#include <Storages/MergeTree/MergeTreeSequentialBlockInputStream.h>
#include <Storages/MergeTree/MergeTreeBlockReadUtils.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int MEMORY_LIMIT_EXCEEDED;
}

MergeTreeSequentialBlockInputStream::MergeTreeSequentialBlockInputStream(
    const MergeTreeData & storage_,
    const MergeTreeData::DataPartPtr & data_part_,
    Names columns_to_read_,
    bool read_with_direct_io_,
    bool quiet)
    : storage(storage_)
    , data_part(data_part_)
    , part_columns_lock(data_part->columns_lock)
    , columns_to_read(columns_to_read_)
    , read_with_direct_io(read_with_direct_io_)
    , mark_cache(storage.context.getMarkCache())
{
    if (!quiet)
        LOG_TRACE(log, "Reading " << data_part->marks_count << " marks from part " << data_part->name
            << ", totaly " << data_part->rows_count
            << " rows starting from the begging of the part");

    addTotalRowsApprox(data_part->rows_count);

    header = storage.getSampleBlockForColumns(columns_to_read);

    fixHeader(header);

    const NamesAndTypesList & physical_columns = storage.getColumns().getAllPhysical();
    auto column_names_with_helper_columns = columns_to_read;
    injectRequiredColumns(storage, data_part, column_names_with_helper_columns);

    reader = std::make_unique<MergeTreeReader>(
        data_part->getFullPath(), data_part, physical_columns.addTypes(column_names_with_helper_columns), /* uncompressed_cache = */ nullptr,
        mark_cache.get(), /* save_marks_in_cache = */ false, storage,
        MarkRanges{MarkRange(0, data_part->marks_count)},
        /* bytes to use AIO */ read_with_direct_io ? 1UL : std::numeric_limits<size_t>::max(),
        DBMS_DEFAULT_BUFFER_SIZE);
}


void MergeTreeSequentialBlockInputStream::fixHeader(Block & header_block) const
{
    /// Types may be different during ALTER (when this stream is used to perform an ALTER).
    for (const auto & name_type : data_part->columns)
    {
        if (header_block.has(name_type.name))
        {
            auto & elem = header_block.getByName(name_type.name);
            if (!elem.type->equals(*name_type.type))
            {
                elem.type = name_type.type;
                elem.column = elem.type->createColumn();
            }
        }
    }
}

Block MergeTreeSequentialBlockInputStream::getHeader() const
{
    return header;
}

Block MergeTreeSequentialBlockInputStream::readImpl()
try
{
    Block res;
    if (!isCancelled() && current_row < data_part->rows_count)
    {
        bool continue_reading = (current_mark != 0);
        size_t rows_readed = reader->readRows(current_mark, continue_reading, storage.index_granularity, res);

        res.checkNumberOfRows();

        current_row += rows_readed;
        current_mark += (rows_readed / storage.index_granularity);
        bool should_reorder = false, should_evaluate_missing_defaults = false;
        reader->fillMissingColumns(res, should_reorder, should_evaluate_missing_defaults, res.rows());

        if (res && should_evaluate_missing_defaults)
            reader->evaluateMissingDefaults(res);

        if (res && should_reorder)
            reader->reorderColumns(res, header.getNames(), nullptr);
    }
    else
    {
        finish();
    }

    return res;
}
catch (...)
{
    /// Suspicion of the broken part. A part is added to the queue for verification.
    if (getCurrentExceptionCode() != ErrorCodes::MEMORY_LIMIT_EXCEEDED)
        storage.reportBrokenPart(data_part->name);
    throw;
}


void MergeTreeSequentialBlockInputStream::finish()
{
    /** Close the files (before destroying the object).
     * When many sources are created, but simultaneously reading only a few of them,
     * buffers don't waste memory.
     */
    reader.reset();
    part_columns_lock.unlock();
    data_part.reset();
}


MergeTreeSequentialBlockInputStream::~MergeTreeSequentialBlockInputStream() = default;

}