/*************************************************************************
*
* Copyright 2021 highstreet technologies GmbH and others
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
***************************************************************************/

#pragma once

#include <sysrepo.h>

int nf_oran_du_init(void);
void nf_oran_du_free(void);

int administrative_state_change_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath,
    sr_event_t event, uint32_t request_id, void *private_data);

void set_sector_visibility(char *cell_id, int cell_state);