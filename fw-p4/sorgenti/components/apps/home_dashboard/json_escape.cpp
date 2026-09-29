#include "json_escape.h"
#include <stdio.h>

std::string json_escape(const char *in)
{
    std::string o = "\"";
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        switch (*p) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            /* Gli altri caratteri di controllo in forma \u00XX: dentro una
               stringa JSON non possono comparire cosi' come sono. Il resto,
               UTF-8 compreso, passa tale e quale. */
            if (*p < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", *p);
                o += b;
            } else {
                o += (char)*p;
            }
        }
    }
    return o + "\"";
}
