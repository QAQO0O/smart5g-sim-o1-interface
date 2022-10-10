/*************************************************************************
*
* Copyright 2022 highstreet technologies GmbH and others
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

#define _GNU_SOURCE

#include "ves_o1_pnf_registration.h"
#include "utils/log_utils.h"
#include "utils/sys_utils.h"
#include "utils/rand_utils.h"
#include "utils/http_client.h"
#include "utils/nts_utils.h"
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#include "core/session.h"
#include "core/framework.h"
#include "core/xpath.h"

static int ves_o1_pnf_sequence_number = 0;
static pthread_t ves_o1_pnf_registration_thread;
static void* ves_o1_pnf_registration_thread_routine(void *arg);
static int ves_o1_pnf_registration_send(sr_session_ctx_t *current_session, const char *nf_ip_v4_address, const char *nf_ip_v6_address, int nf_port, nts_mount_point_addressing_method_t mp, bool is_tls);
static cJSON* ves_create_o1_pnf_registration_fields(const char *nf_ip_v4_address, const char *nf_ip_v6_address, int nf_port, bool is_tls);

static int ves_o1_pnf_registration_status = 0;

int ves_o1_pnf_registration_feature_get_status(void) {
    return ves_o1_pnf_registration_status;
}

int ves_o1_pnf_registration_feature_start(sr_session_ctx_t *current_session) {
    assert(current_session);

    ves_o1_pnf_sequence_number = 0;

    sr_val_t *value = 0;
    int rc = NTS_ERR_OK;
    bool o1_pnf_registration_enabled = false;
    if(strlen(framework_environment.nts.nf_standalone_start_features)) {
        o1_pnf_registration_enabled = true;
    }
    else {
        rc = sr_get_item(current_session, NTS_NF_VES_O1_PNF_REGISTRATION_SCHEMA_XPATH, 0, &value);
        if(rc == SR_ERR_OK) {
            o1_pnf_registration_enabled = value->data.bool_val;
            sr_free_val(value);
        }
        else if(rc != SR_ERR_NOT_FOUND) {
            log_error("sr_get_item failed\n");
            return NTS_ERR_FAILED;
        }
    }

    if(o1_pnf_registration_enabled == false) {
        log_add_verbose(2, "O1 PNF registration (stndDefined) is disabled\n");
        return NTS_ERR_OK;
    }

    if(pthread_create(&ves_o1_pnf_registration_thread, 0, ves_o1_pnf_registration_thread_routine, current_session)) {
        log_error("could not create thread for o1 pnf registration\n");
        return NTS_ERR_FAILED;
    }

    return NTS_ERR_OK;
}

static void* ves_o1_pnf_registration_thread_routine(void *arg) {
    sr_session_ctx_t *current_session = arg;

    int ssh_base_port = 0;
    int tls_base_port = 0;
    char nf_ip_v4_address[128];
    char nf_ip_v6_address[128];

    nf_ip_v4_address[0] = 0;
    nf_ip_v6_address[0] = 0;

    nts_mount_point_addressing_method_t mp = nts_mount_point_addressing_method_get(current_session);
    if(mp == UNKNOWN_MAPPING) {
        log_error("mount-point-addressing-method failed\n");
        return (void*)NTS_ERR_FAILED;
    }
    else if(mp == DOCKER_MAPPING) {
        if (framework_environment.settings.ip_v4 != 0) {
            strcpy(nf_ip_v4_address, framework_environment.settings.ip_v4);
        }
        if (framework_environment.settings.ip_v6 && framework_environment.settings.ip_v6_enabled) {
            strcpy(nf_ip_v6_address, framework_environment.settings.ip_v6);
        }

        ssh_base_port = STANDARD_NETCONF_PORT;
        tls_base_port = ssh_base_port + framework_environment.settings.ssh_connections;
    }
    else {
        if(framework_environment.settings.ip_v6_enabled) {
            strcpy(nf_ip_v6_address, framework_environment.host.ip);
        }
        else {
            strcpy(nf_ip_v4_address, framework_environment.host.ip);
        }

        ssh_base_port = framework_environment.host.ssh_base_port;
        tls_base_port = framework_environment.host.tls_base_port;
    }

    uint32_t total_regs = 0;
    struct regs_s {
        bool sent;
        uint16_t port;
        bool is_tls;
    } *regs;

    regs = (struct regs_s *)malloc(sizeof(struct regs_s) * (1 + framework_environment.settings.ssh_connections + framework_environment.settings.tls_connections));
    if(regs == 0) {
        log_error("malloc failed\n");
        return (void*)NTS_ERR_FAILED;
    }


    if((framework_environment.settings.ssh_connections + framework_environment.settings.tls_connections) > 1) {
        for(int port = ssh_base_port; port < ssh_base_port + framework_environment.settings.ssh_connections; port++) {
            regs[total_regs].sent = false;
            regs[total_regs].port = port;
            regs[total_regs].is_tls = false;
            total_regs++;
        }

        for(int port = tls_base_port; port < tls_base_port + framework_environment.settings.tls_connections; port++) {
            regs[total_regs].sent = false;
            regs[total_regs].port = port;
            regs[total_regs].is_tls = true;
            total_regs++;
        }
    }
    else {
        bool tls;
        if(framework_environment.settings.tls_connections == 0) {
            tls = false;
        }
        else {
            tls = true;
        }

        regs[total_regs].sent = false;
        regs[total_regs].port = 0;
        regs[total_regs].is_tls = tls;
        total_regs++;
    }

    uint32_t remaining = total_regs;
    while(remaining) {
        for(int i = 0; i < total_regs; i++) {
            if(regs[i].sent == false) {
                uint16_t port = regs[i].port;
                bool is_tls = regs[i].is_tls;
                int rc = ves_o1_pnf_registration_send(current_session, nf_ip_v4_address, nf_ip_v6_address, port, mp, is_tls);
                if(rc == NTS_ERR_OK) {
                    remaining--;
                    regs[i].sent = true;
                }
                else {
                    log_error("O1 pnfRegistration (stndDefined) failed for ipv4=%s ipv6=%s port=%d is_tls=%d\n", nf_ip_v4_address, nf_ip_v6_address, port, is_tls);
                }
            }
        }
        if(remaining) {
            log_error("O1 pnfRegistration (stndDefined) could not register all ports; retrying in 5 seconds...\n");
            sleep(5);
        }
    }
    free(regs);
    log_add_verbose(2, "O1 PNF registration (stndDefined) finished\n");
    ves_o1_pnf_registration_status = 1;

    return NTS_ERR_OK;
}

static int ves_o1_pnf_registration_send(sr_session_ctx_t *current_session, const char *nf_ip_v4_address, const char *nf_ip_v6_address, int nf_port, nts_mount_point_addressing_method_t mp, bool is_tls) {
    assert(current_session);

    cJSON *post_data_json = cJSON_CreateObject();
    if(post_data_json == 0) {
        log_error("could not create cJSON object\n");
        return NTS_ERR_FAILED;
    }

    cJSON *event = cJSON_CreateObject();
    if(event == 0) {
        log_error("could not create cJSON object\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    if(cJSON_AddItemToObject(post_data_json, "event", event) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    char *hostname_string = framework_environment.settings.hostname;
    cJSON *common_event_header = ves_create_common_event_header_721("stndDefined", "o1-notify-pnf-registration", hostname_string, nf_port, "Low", ves_o1_pnf_sequence_number++, "o1-notify-pnf-registration");
    if(common_event_header == 0) {
        log_error("could not create cJSON object\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    if(nf_port == 0) {
        if(mp == DOCKER_MAPPING) {
            nf_port = STANDARD_NETCONF_PORT;
        }
        else {
            if(is_tls) {
                nf_port = framework_environment.host.tls_base_port;
            }
            else {
                nf_port = framework_environment.host.ssh_base_port;
            }
        }
    }

    if(cJSON_AddItemToObject(event, "commonEventHeader", common_event_header) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

	cJSON *o1_pnf_registration_fields = ves_create_o1_pnf_registration_fields(nf_ip_v4_address, nf_ip_v6_address, nf_port, is_tls);
    if(o1_pnf_registration_fields == 0) {
        log_error("could not create cJSON object\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    if(cJSON_AddItemToObject(event, "stndDefinedFields", o1_pnf_registration_fields) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    char *post_data = cJSON_PrintUnformatted(post_data_json);
    cJSON_Delete(post_data_json);
    if(post_data == 0) {
        log_error("cJSON_PrintUnformatted failed\n");
        return NTS_ERR_FAILED;
    }


    ves_details_t *ves_details = ves_endpoint_details_get(current_session, 0);
    if(!ves_details) {
        log_error("ves_endpoint_details_get failed\n");
        free(post_data);
        return NTS_ERR_FAILED;
    }

    int rc = http_request(ves_details->url, ves_details->username, ves_details->password, "POST", post_data, 0, 0);
    ves_details_free(ves_details);
    free(post_data);

    if(rc != NTS_ERR_OK) {
        log_error("http_request failed\n");
        return NTS_ERR_FAILED;
    }

    return NTS_ERR_OK;
}

static cJSON* ves_create_o1_pnf_registration_fields(const char *nf_ip_v4_address, const char *nf_ip_v6_address, int nf_port, bool is_tls) {

    cJSON *stnd_defined_fields = cJSON_CreateObject();
    if(stnd_defined_fields == 0) {
        log_error("could not create JSON object\n");
        return 0;
    }

    if(cJSON_AddStringToObject(stnd_defined_fields, "stndDefinedFieldsVersion", "1.0") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(stnd_defined_fields, "schemaReference", "https://gerrit.o-ran-sc.org/r/gitweb?p=scp/oam/modeling.git;a=blob;f=data-model/yang/working/o-ran-sc/template/yes-o1-notify-pnf-registration.yang#/components/schemas/o1-notify-pnf-registration") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    cJSON *data = cJSON_CreateObject();
    if(data == 0) {
        log_error("could not create JSON object\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }
    cJSON_AddItemToObject(stnd_defined_fields, "data", data);

    if(cJSON_AddStringToObject(data, "object-class", "manged-function") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "object-instance", "YES-API-PROVIDER") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "notification-identifier", (double)(0)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "notification-type", "o1-notify-pnf-registration") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    char *current_date_and_time = get_current_date_and_time();
    if(current_date_and_time == 0) {
        log_error("get_current_date_and_time failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "event-time", current_date_and_time) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        free(current_date_and_time);
        return 0;
    }
    free(current_date_and_time);

    if(cJSON_AddStringToObject(data, "system-distinguished-name", "DN:managed-element=YES-API-PROVIDER") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "o1-specification-version", "v07.00") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    char serial_number[512];
    sprintf(serial_number, "%s-%s-%d-Simulated Device Melacon", framework_environment.settings.hostname, nf_ip_v4_address, nf_port);

    if(cJSON_AddStringToObject(data, "serial-number", serial_number) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "vendor-pen", (double)(57272)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if (!framework_environment.settings.ip_v6_enabled) {
        if (nf_ip_v4_address != 0 && strlen(nf_ip_v4_address) > 0) {
            if(cJSON_AddStringToObject(data, "oam-host", nf_ip_v4_address) == 0) {
                log_error("cJSON_AddItemToObject failed\n");
                cJSON_Delete(stnd_defined_fields);
                return 0;
            }
        }
    }
    else {
        if (nf_ip_v6_address != 0 && strlen(nf_ip_v6_address) > 0) {
            if(cJSON_AddStringToObject(data, "oam-host", nf_ip_v6_address) == 0) {
                log_error("cJSON_AddItemToObject failed\n");
                cJSON_Delete(stnd_defined_fields);
                return 0;
            }
        }
    }

    if(cJSON_AddNumberToObject(data, "oam-port", (double)(nf_port)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    char *mac_addr = rand_mac_address();
    if(mac_addr == 0) {
        log_error("rand_mac_address failed\n")
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "mac-address", mac_addr) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        free(mac_addr);
        return 0;
    }
    free(mac_addr);

    if(cJSON_AddStringToObject(data, "unit-family", "O-RAN-SC SIM") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "unit-type", "O1-Interface") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "model-number", "1859a6ea-2520-11ed-861d-0242ac120002") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "software-version", "G-Release") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "restart-reason", "External trigger") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "manufacture-date", "2022-08-26Z") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "last-service-date", "2022-08-26Z") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(data, "username", "netconf") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(is_tls) {
        //TLS specific configuration
        if(cJSON_AddStringToObject(data, "transport-protocol", "tls") == 0) {
            log_error("cJSON_AddItemToObject failed\n");
            cJSON_Delete(stnd_defined_fields);
            return 0;
        }

        if(cJSON_AddStringToObject(data, "key-reference", KS_KEY_NAME) == 0) {
            log_error("cJSON_AddItemToObject failed\n");
            cJSON_Delete(stnd_defined_fields);
            return 0;
        }
    }
    else {
        //SSH specific configuration
        if(cJSON_AddStringToObject(data, "transport-protocol", "ssh") == 0) {
            log_error("cJSON_AddItemToObject failed\n");
            cJSON_Delete(stnd_defined_fields);
            return 0;
        }

        // hardcoded password here
        if(cJSON_AddStringToObject(data, "password", "netconf!") == 0) {
            log_error("cJSON_AddItemToObject failed\n");
            cJSON_Delete(stnd_defined_fields);
            return 0;
        }
    }

    if(cJSON_AddFalseToObject(data, "reconnect-on-changed-schema") == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "connection-timeout", (double)(20000)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "max-connection-attempts", (double)(100)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "between-attempts-timeout", (double)(2000)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "sleep-factor", (double)(1.5)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    if(cJSON_AddNumberToObject(data, "keepalive-delay", (double)(120)) == 0) {
        log_error("cJSON_AddItemToObject failed\n");
        cJSON_Delete(stnd_defined_fields);
        return 0;
    }

    return stnd_defined_fields;
}
