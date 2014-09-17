/*
    Copyright (C) 2012  ABRT Team
    Copyright (C) 2012  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <json.h>
#include "internal_libreport.h"
#include "ureport.h"
#include "libreport_curl.h"

#define CONF_FILE_PATH PLUGINS_CONF_DIR"/ureport.conf"

#define REPORT_URL_SFX "reports/new/"
#define ATTACH_URL_SFX "reports/attach/"
#define BTHASH_URL_SFX "reports/bthash/"

#define RHSM_CERT_PATH "/etc/pki/consumer/cert.pem"
#define RHSM_KEY_PATH "/etc/pki/consumer/key.pem"

#define RHAP_PEM_DIR_PATH "/etc/pki/entitlement"
#define RHAP_ENT_DATA_BEGIN_TAG "-----BEGIN ENTITLEMENT DATA-----"
#define RHAP_ENT_DATA_END_TAG "-----END ENTITLEMENT DATA-----"
#define RHAP_SIG_DATA_BEGIN_TAG "-----BEGIN RSA SIGNATURE-----"
#define RHAP_SIG_DATA_END_TAG "-----END RSA SIGNATURE-----"

#define VALUE_FROM_CONF(opt, var, tr) do { const char *value = getenv("uReport_"opt); \
        if (!value) { value = get_map_string_item_or_NULL(settings, opt); } if (value) { var = tr(value); } \
    } while(0)

static char *puppet_config_print(const char *key)
{
    char *command = xasprintf("puppet config print %s", key);
    char *result = run_in_shell_and_save_output(0, command, NULL, NULL);
    free(command);

    /* run_in_shell_and_save_output always returns non-NULL */
    if (result[0] != '/')
        goto error;

    char *newline = strchrnul(result, '\n');
    if (!newline)
        goto error;

    *newline = '\0';
    return result;
error:
    free(result);
    error_msg_and_die("Unable to determine puppet %s path (puppet not installed?)", key);
}

static void parse_client_auth_paths(struct ureport_server_config *config, const char *client_auth)
{
    if (client_auth == NULL)
        return;

    if (strcmp(client_auth, "") == 0)
    {
        config->ur_client_cert = NULL;
        config->ur_client_key = NULL;
        log_notice("Not using client authentication");
    }
    else if (strcmp(client_auth, "rhsm") == 0)
    {
        config->ur_client_cert = xstrdup(RHSM_CERT_PATH);
        config->ur_client_key = xstrdup(RHSM_KEY_PATH);
    }
    else if (strcmp(client_auth, "rhsm-entitlement") == 0)
    {
        GList *certs = get_file_list(RHAP_PEM_DIR_PATH, "pem");
        if (g_list_length(certs) != 2)
        {
            log_notice(RHAP_PEM_DIR_PATH" does not contain unique cert-key files pair");
            log_notice("Not using client authentication");
            return;
        }

        const char *cert = NULL;
        const char *key = NULL;

        file_obj_t *fst = (file_obj_t *)certs->data;
        file_obj_t *scn = (file_obj_t *)certs->next->data;

        if (strlen(fo_get_filename(fst)) < strlen(fo_get_filename(scn)))
        {
            cert = fo_get_filename(fst);
            key = fo_get_filename(scn);

            config->ur_client_cert = xstrdup(fo_get_fullpath(fst));
            config->ur_client_key = xstrdup(fo_get_fullpath(scn));
        }
        else
        {
            cert = fo_get_filename(scn);
            key = fo_get_filename(fst);

            config->ur_client_cert = xstrdup(fo_get_fullpath(scn));
            config->ur_client_key = xstrdup(fo_get_fullpath(fst));
        }

        const bool iscomplement = prefixcmp(key, cert) != 0 || strcmp("-key", key + strlen(cert)) != 0;
        g_list_free_full(certs, (GDestroyNotify)free_file_obj);

        if (iscomplement)
        {
            log_notice("Key file '%s' isn't complement to cert file '%s'",
                    config->ur_client_key, config->ur_client_cert);
            log_notice("Not using client authentication");

            free(config->ur_client_cert);
            free(config->ur_client_key);
            config->ur_client_cert = NULL;
            config->ur_client_key = NULL;

            return;
        }

        char *certdata = xmalloc_open_read_close(config->ur_client_cert, /*no size limit*/NULL);
        if (certdata != NULL)
        {
            char *ent_data = xstrdup_between(certdata,
                    RHAP_ENT_DATA_BEGIN_TAG, RHAP_ENT_DATA_END_TAG);

            char *sig_data = xstrdup_between(certdata,
                    RHAP_SIG_DATA_BEGIN_TAG, RHAP_SIG_DATA_END_TAG);

            if (ent_data != NULL && sig_data != NULL)
            {
                ent_data = strremovech(ent_data, '\n');
                insert_map_string(config->ur_http_headers,
                        xstrdup("X-RH-Entitlement-Data"),
                        xasprintf(RHAP_ENT_DATA_BEGIN_TAG"%s"RHAP_ENT_DATA_END_TAG, ent_data));

                sig_data = strremovech(sig_data, '\n');
                insert_map_string(config->ur_http_headers,
                        xstrdup("X-RH-Entitlement-Sig"),
                        xasprintf(RHAP_SIG_DATA_BEGIN_TAG"%s"RHAP_SIG_DATA_END_TAG, sig_data));
            }
            else
            {
                log_notice("Cert file '%s' doesn't contain Entitlement and RSA Signature sections", config->ur_client_cert);
                log_notice("Not using HTTP authentication headers");
            }

            free(sig_data);
            free(ent_data);
            free(certdata);
        }
    }
    else if (strcmp(client_auth, "puppet") == 0)
    {
        config->ur_client_cert = puppet_config_print("hostcert");
        config->ur_client_key = puppet_config_print("hostprivkey");
    }
    else
    {
        char *scratch = xstrdup(client_auth);
        config->ur_client_cert = xstrdup(strtok(scratch, ":"));
        config->ur_client_key = xstrdup(strtok(NULL, ":"));
        free(scratch);
        if (config->ur_client_cert == NULL || config->ur_client_key == NULL)
            error_msg_and_die("Invalid client authentication specification");
    }

    if (config->ur_client_cert && config->ur_client_key)
    {
        log_notice("Using client certificate: %s", config->ur_client_cert);
        log_notice("Using client private key: %s", config->ur_client_key);
    }
}

/*
 * Loads uReport configuration from various sources.
 *
 * Replaces a value of an already configured option only if the
 * option was found in a configuration source.
 *
 * @param config a server configuration to be populated
 */
static void load_ureport_server_config(struct ureport_server_config *config, map_string_t *settings)
{
    VALUE_FROM_CONF("URL", config->ur_url, (const char *));
    VALUE_FROM_CONF("SSLVerify", config->ur_ssl_verify, string_to_bool);

    bool include_auth = false;
    VALUE_FROM_CONF("IncludeAuthData", include_auth, string_to_bool);

    if (include_auth)
    {
        const char *auth_items = NULL;
        VALUE_FROM_CONF("AuthDataItems", auth_items, (const char *));
        config->ur_prefs.urp_auth_items = parse_list(auth_items);

        if (config->ur_prefs.urp_auth_items == NULL)
            log_warning("IncludeAuthData set to 'yes' but AuthDataItems is empty.");
    }

    const char *client_auth = NULL;
    VALUE_FROM_CONF("SSLClientAuth", client_auth, (const char *));
    parse_client_auth_paths(config, client_auth);
}

struct ureport_server_response {
    bool is_error;
    char *value;
    char *message;
    char *bthash;
    GList *reported_to_list;
    char *solution;
};

void free_ureport_server_response(struct ureport_server_response *resp)
{
    if (!resp)
        return;

    free(resp->solution);
    g_list_free_full(resp->reported_to_list, g_free);
    free(resp->bthash);
    free(resp->message);
    free(resp->value);
    free(resp);
}

static char *parse_solution_from_json_list(struct json_object *list, GList **reported_to)
{
    json_object *list_elem, *struct_elem;
    const char *cause, *note, *url;
    struct strbuf *solution_buf = strbuf_new();

    const unsigned length = json_object_array_length(list);

    const char *one_format = _("Your problem seems to be caused by %s\n\n%s\n");
    if (length > 1)
    {
        strbuf_append_str(solution_buf, _("Your problem seems to be caused by one of the following:\n"));
        one_format = "\n* %s\n\n%s\n";
    }

    bool empty = true;
    for (unsigned i = 0; i < length; ++i)
    {
        list_elem = json_object_array_get_idx(list, i);
        if (!list_elem)
            continue;

        if (!json_object_object_get_ex(list_elem, "cause", &struct_elem))
            continue;

        cause = json_object_get_string(struct_elem);
            continue;

        if (!json_object_object_get_ex(list_elem, "note", &struct_elem))
            continue;

        note = json_object_get_string(struct_elem);
        if (!note)
            continue;

        empty = false;
        strbuf_append_strf(solution_buf, one_format, cause, note);

        if (!json_object_object_get_ex(list_elem, "url", &struct_elem))
            continue;

        url = json_object_get_string(struct_elem);
        if (url)
        {
            char *reported_to_line = xasprintf("%s: URL=%s", cause, url);
            *reported_to = g_list_append(*reported_to, reported_to_line);
        }
    }

    if (empty)
    {
        strbuf_free(solution_buf);
        return NULL;
    }

    return strbuf_free_nobuf(solution_buf);
}

/* reported_to json element should be a list of structures
{ "reporter": "Bugzilla",
  "type": "url",
  "value": "https://bugzilla.redhat.com/show_bug.cgi?id=XYZ" } */
static GList *parse_reported_to_from_json_list(struct json_object *list)
{
    int i;
    json_object *list_elem, *struct_elem;
    const char *reporter, *value, *type;
    char *reported_to_line, *prefix;
    GList *result = NULL;

    for (i = 0; i < json_object_array_length(list); ++i)
    {
        prefix = NULL;
        list_elem = json_object_array_get_idx(list, i);
        if (!list_elem)
            continue;

        if (!json_object_object_get_ex(list_elem, "reporter", &struct_elem))
            continue;

        reporter = json_object_get_string(struct_elem);
        if (!reporter)
            continue;

        if (!json_object_object_get_ex(list_elem, "value", &struct_elem))
            continue;

        value = json_object_get_string(struct_elem);
        if (!value)
            continue;

        if (!json_object_object_get_ex(list_elem, "type", &struct_elem))
            continue;

        type = json_object_get_string(struct_elem);
        if (type)
        {
            if (strcasecmp("url", type) == 0)
                prefix = xstrdup("URL=");
            else if (strcasecmp("bthash", type) == 0)
                prefix = xstrdup("BTHASH=");
        }

        if (!prefix)
            prefix = xstrdup("");

        reported_to_line = xasprintf("%s: %s%s", reporter, prefix, value);
        free(prefix);

        result = g_list_append(result, reported_to_line);
    }

    return result;
}

/*
 * Reponse samples:
 * {"error":"field 'foo' is required"}
 * {"response":"true"}
 * {"response":"false"}
 */
static struct ureport_server_response *ureport_server_parse_json(json_object *json)
{
    json_object *obj = NULL;
    if (json_object_object_get_ex(json, "error", &obj))
    {
        struct ureport_server_response *out_response = xzalloc(sizeof(*out_response));
        out_response->is_error = true;
        /*
         * Used to use json_object_to_json_string(obj), but it returns
         * the string in quote marks (") - IOW, json-formatted string.
         */
        out_response->value = xstrdup(json_object_get_string(obj));
        return out_response;
    }

    if (json_object_object_get_ex(json, "result", &obj))
    {
        struct ureport_server_response *out_response = xzalloc(sizeof(*out_response));
        out_response->value = xstrdup(json_object_get_string(obj));

        json_object *message = NULL;
        if (json_object_object_get_ex(json, "message", &message))
            out_response->message = xstrdup(json_object_get_string(message));

        json_object *bthash = NULL;
        if (json_object_object_get_ex(json, "bthash", &bthash))
            out_response->bthash = xstrdup(json_object_get_string(bthash));

        json_object *reported_to_list = NULL;
        if (json_object_object_get_ex(json, "reported_to", &reported_to_list))
            out_response->reported_to_list = parse_reported_to_from_json_list(reported_to_list);

        json_object *solutions = NULL;
        if (json_object_object_get_ex(json, "solutions", &solutions))
            out_response->solution = parse_solution_from_json_list(solutions, &(out_response->reported_to_list));

        return out_response;
    }

    return NULL;
}

static struct ureport_server_response *get_server_response(post_state_t *post_state, struct ureport_server_config *config)
{
    /* Previously, the condition here was (post_state->errmsg[0] != '\0')
     * however when the server asks for optional client authentication and we do not have the certificates,
     * then post_state->errmsg contains "NSS: client certificate not found (nickname not specified)" even though
     * the request succeeded.
     */
    if (post_state->curl_result != CURLE_OK)
    {
        error_msg(_("Failed to upload uReport to the server '%s' with curl: %s"), config->ur_url, post_state->errmsg);
        return NULL;
    }

    if (post_state->http_resp_code == 404)
    {
        error_msg(_("The URL '%s' does not exist (got error 404 from server)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code == 500)
    {
        error_msg(_("The server at '%s' encountered an internal error (got error 500)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code == 503)
    {
        error_msg(_("The server at '%s' currently can't handle the request (got error 503)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code != 202
            && post_state->http_resp_code != 400
            && post_state->http_resp_code != 413)
    {
        /* can't print better error message */
        error_msg(_("Unexpected HTTP response from '%s': %d"), config->ur_url, post_state->http_resp_code);
        log_notice("%s", post_state->body);
        return NULL;
    }

    json_object *const json = json_tokener_parse(post_state->body);

    if (is_error(json))
    {
        error_msg(_("Unable to parse response from ureport server at '%s'"), config->ur_url);
        log_notice("%s", post_state->body);
        json_object_put(json);
        return NULL;
    }

    struct ureport_server_response *response = ureport_server_parse_json(json);
    json_object_put(json);

    if (!response)
        error_msg(_("The response from '%s' has invalid format"), config->ur_url);
    else if ((post_state->http_resp_code == 202 && response->is_error)
                || (post_state->http_resp_code != 202 && !response->is_error))
    {
        /* HTTP CODE 202 means that call was successful but the response */
        /* has an error message */
        error_msg(_("Type mismatch has been detected in the response from '%s'"), config->ur_url);
    }

    return response;
}

typedef post_state_t *(*attach_handler)(const char *, void *, struct ureport_server_config *);

static post_state_t *wrp_ureport_attach_rhbz(const char *ureport_hash, int *rhbz_bug,
        struct ureport_server_config *config)
{
    return ureport_attach_rhbz(ureport_hash, *rhbz_bug, config);
}

static bool perform_attach(struct ureport_server_config *config, const char *ureport_hash,
        attach_handler handler, void *args)
{
    char *dest_url = concat_path_file(config->ur_url, ATTACH_URL_SFX);
    const char *old_url = config->ur_url;
    config->ur_url = dest_url;
    post_state_t *post_state = handler(ureport_hash, args, config);
    config->ur_url = old_url;
    free(dest_url);

    struct ureport_server_response *resp = get_server_response(post_state, config);
    free_post_state(post_state);
    /* don't use str_bo_bool() because we require "true" string */
    const int result = !resp || resp->is_error || strcmp(resp->value,"true") != 0;

    if (resp && resp->is_error)
    {
        error_msg(_("The server at '%s' responded with an error: '%s'"), config->ur_url, resp->value);
    }

    free_ureport_server_response(resp);

    return result;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
#if ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

    abrt_init(argv);

    struct ureport_server_config config = {
        .ur_url = NULL,
        .ur_ssl_verify = true,
        .ur_client_cert = NULL,
        .ur_client_key = NULL,
        .ur_http_headers = NULL,
        {
            .urp_auth_items = NULL,
        },
    };

    config.ur_http_headers = new_map_string();

    enum {
        OPT_v = 1 << 0,
        OPT_d = 1 << 1,
        OPT_u = 1 << 2,
        OPT_k = 1 << 3,
        OPT_t = 1 << 4,
        OPT_i = 1 << 5,
    };

    int ret = 1; /* "failure" (for now) */
    bool insecure = !config.ur_ssl_verify;
    const char *conf_file = CONF_FILE_PATH;
    const char *arg_server_url = NULL;
    const char *client_auth = NULL;
    GList *auth_items = NULL;
    const char *dump_dir_path = ".";
    const char *ureport_hash = NULL;
    bool ureport_hash_from_rt = false;
    int rhbz_bug = -1;
    bool rhbz_bug_from_rt = false;
    const char *email_address = NULL;
    bool email_address_from_env = false;
    char *comment = NULL;
    bool comment_file = NULL;
    struct dump_dir *dd = NULL;
    struct options program_options[] = {
        OPT__VERBOSE(&g_verbose),
        OPT__DUMP_DIR(&dump_dir_path),
        OPT_STRING('u', "url", &arg_server_url, "URL", _("Specify server URL")),
        OPT_BOOL('k', "insecure", &insecure,
                          _("Allow insecure connection to ureport server")),
        OPT_STRING('t', "auth", &client_auth, "SOURCE", _("Use client authentication")),
        OPT_LIST('i', "auth_items", &auth_items, "AUTH_ITEMS", _("Additional files included in 'auth' key")),
        OPT_STRING('c', NULL, &conf_file, "FILE", _("Configuration file")),
        OPT_STRING('a', "attach", &ureport_hash, "BTHASH",
                          _("bthash of uReport to attach (conflicts with -A)")),
        OPT_BOOL('A', "attach-rt", &ureport_hash_from_rt,
                          _("attach to a bthash from reported_to (conflicts with -a)")),
        OPT_STRING('e', "email", &email_address, "EMAIL",
                          _("contact e-mail address (requires -a|-A, conflicts with -E)")),
        OPT_BOOL('E', "email-env", &email_address_from_env,
                          _("contact e-mail address from environment or configuration file (requires -a|-A, conflicts with -e)")),
        OPT_INTEGER('b', "bug-id", &rhbz_bug,
                          _("attach RHBZ bug (requires -a|-A, conflicts with -B)")),
        OPT_BOOL('B', "bug-id-rt", &rhbz_bug_from_rt,
                          _("attach last RHBZ bug from reported_to (requires -a|-A, conflicts with -b)")),
        OPT_STRING('o', "comment", &comment, "DESCRIPTION",
                          _("attach short text (requires -a|-A, conflicts with -D)")),
        OPT_BOOL('O', "comment-file", &comment_file,
                          _("attach short text from comment (requires -a|-A, conflicts with -d)")),
        OPT_END(),
    };

    const char *program_usage_string = _(
        "& [-v] [-c FILE] [-u URL] [-k] [-t SOURCE] [-i AUTH_ITEMS]\\\n"
        "  [-A -a bthash -B -b bug-id -E -e email -O -o comment] [-d DIR]\n"
        "\n"
        "Upload micro report or add an attachment to a micro report\n"
        "\n"
        "Reads the default configuration from "CONF_FILE_PATH
    );

    unsigned opts = parse_opts(argc, argv, program_options, program_usage_string);

    map_string_t *settings = new_map_string();
    load_conf_file(conf_file, settings, /*skip key w/o values:*/ false);

    load_ureport_server_config(&config, settings);

    if (opts & OPT_u)
        config.ur_url = arg_server_url;
    if (opts & OPT_k)
        config.ur_ssl_verify = !insecure;
    if (opts & OPT_t)
        parse_client_auth_paths(&config, client_auth);
    if (opts & OPT_i)
    {
        g_list_free_full(config.ur_prefs.urp_auth_items, free);
        config.ur_prefs.urp_auth_items = auth_items;
    }

    if (!config.ur_url)
        error_msg_and_die("You need to specify server URL");

    post_state_t *post_state = NULL;

    if (ureport_hash && ureport_hash_from_rt)
        error_msg_and_die("You need to pass either -a bthash or -A");

    if (rhbz_bug >= 0 && rhbz_bug_from_rt)
        error_msg_and_die("You need to pass either -b bug-id or -B");

    if (email_address && email_address_from_env)
        error_msg_and_die("You need to pass either -e bthash or -E");

    if (comment && comment_file)
        error_msg_and_die("You need to pass either -o comment or -O");

    if (ureport_hash_from_rt || rhbz_bug_from_rt || comment_file)
    {
        dd = dd_opendir(dump_dir_path, DD_OPEN_READONLY);
        if (!dd)
            xfunc_die();

        if (ureport_hash_from_rt)
        {
            report_result_t *ureport_result = find_in_reported_to(dd, "uReport");

            if (!ureport_result || !ureport_result->bthash)
                error_msg_and_die(_("This problem does not have an uReport assigned."));

            /* sorry, this will be leaked */
            ureport_hash = xstrdup(ureport_result->bthash);

            free_report_result(ureport_result);
        }

        if (rhbz_bug_from_rt)
        {
            report_result_t *bz_result = find_in_reported_to(dd, "Bugzilla");

            if (!bz_result || !bz_result->url)
                error_msg_and_die(_("This problem has not been reported to Bugzilla."));

            char *bugid_ptr = strstr(bz_result->url, "show_bug.cgi?id=");
            if (!bugid_ptr)
                error_msg_and_die(_("Unable to find bug ID in bugzilla URL '%s'"), bz_result->url);
            bugid_ptr += strlen("show_bug.cgi?id=");

            /* we're just reading int, sscanf works fine */
            if (sscanf(bugid_ptr, "%d", &rhbz_bug) != 1)
                error_msg_and_die(_("Unable to parse bug ID from bugzilla URL '%s'"), bz_result->url);

            free_report_result(bz_result);
        }

        if (comment_file)
        {
            comment = dd_load_text(dd, FILENAME_COMMENT);
            if (comment == NULL)
                error_msg_and_die(_("Cannot attach comment from 'comment' file"));
            if (comment[0] == '\0')
                error_msg_and_die(_("'comment' file is empty"));
        }

        dd_close(dd);
    }

    if (email_address_from_env)
    {
        VALUE_FROM_CONF("ContactEmail", email_address, (const char *));

        if (!email_address)
            error_msg_and_die(_("Neither environment variable 'uReport_ContactEmail' nor configuration option 'ContactEmail' is set"));
    }

    if (ureport_hash)
    {
        if (rhbz_bug < 0 && !email_address && !comment)
            error_msg_and_die(_("You need to specify bug ID, contact email, comment or all of them"));

        if (rhbz_bug >= 0)
        {
            if (perform_attach(&config, ureport_hash, (attach_handler)wrp_ureport_attach_rhbz, (void *)&rhbz_bug))
                goto finalize;
        }

        if (email_address)
        {
            if (perform_attach(&config, ureport_hash, (attach_handler)ureport_attach_email, (void *)email_address))
                goto finalize;
        }

        if (comment)
        {
            if (perform_attach(&config, ureport_hash, (attach_handler)ureport_attach_comment, (void *)comment))
                goto finalize;
        }

        ret = 0;
        goto finalize;
    }
    if (!ureport_hash && (rhbz_bug >= 0 || email_address))
        error_msg_and_die(_("You need to specify bthash of the uReport to attach."));

    /* -b, -a nor -r were specified - upload uReport from dump_dir */
    const char *server_url = config.ur_url;
    char *dest_url = concat_path_file(config.ur_url, REPORT_URL_SFX);
    config.ur_url = dest_url;

    char *json_ureport = ureport_from_dump_dir_ext(dump_dir_path, &(config.ur_prefs));
    if (!json_ureport)
    {
        error_msg(_("Not uploading an empty uReport"));
        goto format_err;
    }

    post_state = post_ureport(json_ureport, &config);
    free(json_ureport);

    if (!post_state)
    {
        error_msg(_("Failed on submitting the problem"));
        goto format_err;
    }

    struct ureport_server_response *response = get_server_response(post_state, &config);

    if (!response)
        goto format_err;

    if (!response->is_error)
    {
        log_notice("is known: %s", response->value);
        ret = 0; /* "success" */

        dd = dd_opendir(dump_dir_path, /* flags */ 0);
        if (!dd)
            xfunc_die();

        if (response->bthash)
        {
            char *msg = xasprintf("uReport: BTHASH=%s", response->bthash);
            add_reported_to(dd, msg);
            free(msg);

            char *bthash_url = concat_path_file(server_url, BTHASH_URL_SFX);
            msg = xasprintf("ABRT Server: URL=%s%s", bthash_url, response->bthash);
            add_reported_to(dd, msg);
            free(msg);
            free(bthash_url);
        }

        if (response->reported_to_list)
        {
            for (GList *e = response->reported_to_list; e; e = g_list_next(e))
                add_reported_to(dd, e->data);
        }

        if (response->solution)
            dd_save_text(dd, FILENAME_NOT_REPORTABLE, response->solution);

        dd_close(dd);

        /* If a reported problem is not known then emit NEEDMORE */
        if (strcmp("true", response->value) == 0)
        {
            log(_("This problem has already been reported."));
            if (response->message)
                log(response->message);

            ret = EXIT_STOP_EVENT_RUN;
        }
    }
    else
    {
        error_msg(_("Server responded with an error: '%s'"), response->value);
    }

    free_ureport_server_response(response);

format_err:
    free_post_state(post_state);
    free(dest_url);

finalize:
    if (config.ur_prefs.urp_auth_items != auth_items)
        g_list_free_full(config.ur_prefs.urp_auth_items, free);

    free_map_string(config.ur_http_headers);

    free_map_string(settings);
    free(config.ur_client_cert);
    free(config.ur_client_key);

    return ret;
}
