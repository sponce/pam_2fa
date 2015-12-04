#ifdef HAVE_CONFIG_H
    #include "config.h"
#endif

#include <curl/curl.h>

#include "pam_2fa.h"

#define SOAP_REQUEST_TEMPL "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope xmlns:ns0=\"http://cern.ch/GoogleAuthenticator/\" xmlns:ns1=\"http://schemas.xmlsoap.org/soap/envelope/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"><SOAP-ENV:Header/><ns1:Body><ns0:CheckUser><ns0:login>%s</ns0:login><ns0:pincode>%s</ns0:pincode></ns0:CheckUser></ns1:Body></SOAP-ENV:Envelope>"

#define HTTP_BUF_LEN 2048

struct response_curl {
    char buffer[HTTP_BUF_LEN];
    size_t size;
};


static size_t writefunc_curl (char *ptr, size_t size, size_t nmemb, void *userdata);

int gauth_auth_func (pam_handle_t * pamh, user_config * user_cfg, module_config * cfg, char *otp)
{
    CURL *curlh = NULL;
    char *p = NULL, *result = NULL;
    char soap_action[1024], soap_result_tag[1024], soap_result_ok[1024];
    char http_request[HTTP_BUF_LEN] = { 0 }, curl_error[CURL_ERROR_SIZE] = { 0 };
    struct response_curl http_response = { .size = 0 };
    int retval = 0, trial = 0, i = 0;
    struct curl_slist *header_list = NULL;

    p = strrchr(cfg->gauth_ws_action, '/');
    if (!p || !*++p) {
        DBG(("Invalid WS action"));
        pam_syslog(pamh, LOG_ERR, "Invalid WS action: %s", cfg->gauth_ws_action);
        goto done;
    }
    snprintf(soap_result_tag, 1024, "<%sResult>", p);
    snprintf(soap_result_ok, 1024, "<%sResult>true</%sResult>", p, p);
    snprintf(soap_action, 1024, "SOAPAction: \"%s\"", cfg->gauth_ws_action);

    //CURL INITIALIZATION
    curlh = curl_easy_init();
    header_list = curl_slist_append(header_list, "Content-Type: text/xml; charset=utf-8");
    header_list = curl_slist_append(header_list, soap_action);

    retval = curl_easy_setopt(curlh, CURLOPT_FAILONERROR, 1);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }
    retval = curl_easy_setopt(curlh, CURLOPT_ERRORBUFFER, curl_error);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }
    if (cfg->capath) {
        retval = curl_easy_setopt(curlh, CURLOPT_CAPATH, cfg->capath);
        if (retval != CURLE_OK) {
            DBG(("Unable to set CURL options"));
            pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
            goto done;
        }
    }

    retval = curl_easy_setopt(curlh, CURLOPT_HTTPHEADER, header_list);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }
    retval = curl_easy_setopt(curlh, CURLOPT_URL, cfg->gauth_ws_uri);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }
    retval = curl_easy_setopt(curlh, CURLOPT_WRITEFUNCTION, &writefunc_curl);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }
    retval = curl_easy_setopt(curlh, CURLOPT_WRITEDATA, &http_response);
    if (retval != CURLE_OK) {
        DBG(("Unable to set CURL options"));
        pam_syslog(pamh, LOG_ERR, "Unable to set CURL options: %s", curl_error);
        goto done;
    }

    if (!user_cfg->gauth_login[0]) {
        strncpy(user_cfg->gauth_login, "INVALID&&USER&&NAME", GAUTH_LOGIN_LEN);
    }

    //GET USER INPUT
    retval = ERROR;
    for (trial = 0; retval != OK && trial < cfg->retry; ++trial) {
        if(!otp)
            pam_prompt(pamh, PAM_PROMPT_ECHO_ON, &otp, "OTP: ");

        if (otp) {
            DBG(("OTP = %s", otp));

            // VERIFY IF VALID INPUT !
            retval = 1;
            for (i = 0; retval && i < (int) cfg->otp_length; ++i) {
                if (!isdigit(otp[i]))
                    retval = 0;
            }
            if (!retval || otp[i]) {
                DBG(("INCORRRECT code from user!"));
                retval = ERROR;
                free(otp);
                otp = NULL;
                continue;
            }

            snprintf(http_request, HTTP_BUF_LEN, SOAP_REQUEST_TEMPL, user_cfg->gauth_login, otp);
            bzero(otp, cfg->otp_length);
            free(otp);
            otp = NULL;

            retval = curl_easy_setopt(curlh, CURLOPT_POSTFIELDS, http_request);
            if (retval != CURLE_OK) {
                DBG(("Unable to set CURL POST request"));
                pam_syslog(pamh, LOG_ERR, "Unable to set CURL POST request: %s", curl_error);
                goto done;
            }

            retval = curl_easy_perform(curlh);
            bzero(http_request, HTTP_BUF_LEN);

            switch (retval) {
            case 0:
                break;

            default:
                DBG(("curl return value (%d): %s", retval, curl_error));
                retval = ERROR;
                goto done;
            }

            //PARSE THE RESPONSE
            http_response.buffer[http_response.size] = 0;
            http_response.size = 0;
            result = strstr(http_response.buffer, soap_result_tag);
            if (result == NULL) {
                DBG(("Invalid SOAP response: %s", http_response.buffer));
                pam_syslog(pamh, LOG_ERR, "Invalid SOAP response: %s", http_response.buffer);
                retval = ERROR;
                goto done;
            }

            retval = strncmp(result, soap_result_ok, strlen(soap_result_ok));
            if (!retval)
                retval = OK;
            else
  		retval = ERROR;
        } else {
            pam_syslog(pamh, LOG_INFO, "No user input!");
            retval = ERROR;
        }
    }

    done:

    if (otp) free(otp);
    if (curlh) curl_easy_cleanup(curlh);
    if (header_list) curl_slist_free_all(header_list);

    retval = retval == OK ? PAM_SUCCESS : PAM_AUTH_ERR;
    return retval;
}

static size_t writefunc_curl (char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct response_curl *response = (struct response_curl *) userdata;
    size_t handled;

    if (size * nmemb > HTTP_BUF_LEN - response->size - 1)
        return 0;

    handled = size * nmemb;
    memcpy(response->buffer + response->size, ptr, handled);
    response->size += handled;

    return handled;
}
