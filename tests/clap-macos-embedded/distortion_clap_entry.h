//
// Created by Paul Walker on 2/21/25.
//

#pragma once

extern bool dist_entry_init(const char *plugin_path);
extern void dist_entry_deinit(void);
extern const void *dist_entry_get_factory(const char *factory_id);