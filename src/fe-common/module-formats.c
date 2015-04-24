// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

#include "robustirc.h"
#include "common.h"
#include "formats.h"

FORMAT_REC fe_robustirc_formats[] = {
    {MODULE_NAME, "RobustIRC", 0, {0}},

    {NULL, "Errors", 0, {0}},

    {"error_temporary", "{hilight RobustIRC:} Temporary error {reason $0}", 1, {0}},
    {"error_retry", "{hilight RobustIRC:} Retrying request $0 (failed on {server $1}) on {server $2}", 3, {0}},
    {"error_parse_json", "{hilight RobustIRC:} Error parsing chunk \"$0\" as JSON {reason $1}", 2, {0}},
    {"error_permanent", "{hilight RobustIRC:} Permanent error (killed?) {reason $0}", 1, {0}},

    {NULL, NULL, 0, {0}},
};
