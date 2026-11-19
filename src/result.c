#include "minidb.h"
#include <stdlib.h>
#include <string.h>
void md_event_destroy(MdEvent *e) {
    if (!e)
        return;
    if (e->result.columns)
        for (size_t i = 0; i < e->result.column_count; i++)
            free(e->result.columns[i].name);
    if (e->result.values) {
        size_t count = e->result.row_count * e->result.column_count;
        for (size_t i = 0; i < count; i++)
            if (e->result.values[i].type == MD_TEXT) free(e->result.values[i].as.text.ptr);
        free(e->result.values);
    }
    if (e->result.tables)
        for (size_t i = 0; i < e->result.table_count; i++) {
            free(e->result.tables[i].name);
            free(e->result.tables[i].action);
        }
    free(e->result.columns);
    free(e->result.tables);
    free(e->result.output_path);
    memset(e, 0, sizeof *e);
}
