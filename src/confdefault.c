/*
 * The built-in configuration as one text. See confdefault.h.
 */
#include <stdio.h>

#include "confdefault.h"
#include "keys.h"
#include "launchers.h"
#include "metrics.h"
#include "theme.h"

const char *lm_config_default_text(void)
{
    static char text[32768];
    if (!text[0])
        snprintf(text, sizeof(text), "%s\n%s\n%s\n%s", lm_launch_default_conf(), lm_keys_default_conf(), lm_theme_default_conf(),
                 lm_metrics_default_conf());
    return text;
}
