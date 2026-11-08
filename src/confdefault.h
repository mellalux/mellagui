#ifndef MELLAGUI_CONFDEFAULT_H
#define MELLAGUI_CONFDEFAULT_H

/* The whole built-in mellagui.conf as text: [commands], [dock], [dock-apps], [keys], [theme], [windows] ... [notifications], every key with its default.
 * `mellagui --print-config` prints it and the settings key `config-create` writes it to the user's file. */
const char *lm_config_default_text(void);

#endif
