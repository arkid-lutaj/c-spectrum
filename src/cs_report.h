/*
 * cs_report.h - what came out of a run.
 *
 * The text report is for a person reading a terminal. The JSON export is the
 * same run in a form something else can draw, which is how the demo page gets
 * real numbers in it instead of made up ones.
 */

#ifndef CS_REPORT_H
#define CS_REPORT_H

#include "cs_engine.h"
#include <stdio.h>

/* Human readable summary. */
void cs_report_print(const CsEngine *e, const char *source_name, FILE *out);

/* Writes the run to `path` as JSON. Returns false if the file won't open. */
bool cs_report_write_json(const CsEngine *e, const char *source_name,
                          const char *path);

#endif /* CS_REPORT_H */
