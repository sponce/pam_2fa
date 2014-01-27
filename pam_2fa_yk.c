#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <ykclient.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>

#include "pam_2fa.h"


int yk_auth_func(pam_handle_t * pamh, user_config * user_cfg, module_config * cfg, char *otp) {
    ykclient_t *ykc = NULL;
    char **publicid = NULL;
    int retval = 0, trial = 0;

    retval = ykclient_init(&ykc);
    if (retval != YKCLIENT_OK) {
	DBG(("ykclient_init() failed (%d): %s", retval,
	     ykclient_strerror(retval)));
	retval = PAM_AUTHINFO_UNAVAIL;
	goto done;
    }

    retval = ykclient_set_client_hex(ykc, cfg->yk_id, cfg->yk_key);
    if (retval != YKCLIENT_OK) {
	DBG(("ykclient_set_client_b64() failed (%d): %s", retval,
	     ykclient_strerror(retval)));
	retval = PAM_AUTHINFO_UNAVAIL;
	goto done;
    }

    if (cfg->yk_key)
	ykclient_set_verify_signature(ykc, 1);

    if (cfg->capath)
	ykclient_set_ca_path(ykc, cfg->capath);

    if (cfg->yk_uri)
	ykclient_set_url_template(ykc, cfg->yk_uri);

    //GET USER INPUT
    retval = ERROR;
    for (trial = 0; retval != OK && trial < cfg->retry; ++trial) {
        if(!otp) {
	    retval = converse(pamh, cfg, "Yubikey: ", PAM_PROMPT_ECHO_ON, &otp);
	    if (retval != OK) {
	        syslog(LOG_INFO, "PAM converse error");
	        return (ERROR);
	    }
	}

	if (otp) {
	    DBG(("Yubikey = %s", otp));
	    syslog(LOG_DEBUG, "Yubikey OTP: %s (%zu)", otp, strlen(otp));

	    // VERIFY IF VALID INPUT !
	    if (strlen(otp) != YK_OTP_LEN) {
		DBG(("INCORRRECT code from user!"));
	        syslog(LOG_INFO, "Yubikey OTP is incorrect: %s", otp);
		retval = ERROR;
		free(otp);
                otp = NULL;
		continue;
	    }

            for(retval = 1, publicid = user_cfg->yk_publicids; retval && *publicid; ++publicid)
	        retval = strncmp(otp, *publicid, YK_PUBLICID_LEN);

	    if (retval != 0) {
		DBG(("INCORRECT yubikey public ID"));
	        syslog(LOG_INFO, "Yubikey OTP doesn't match user public ids");
		retval = ERROR;
		free(otp);
                otp = NULL;
		continue;
	    }

	    retval = ykclient_request(ykc, otp);
	    DBG(("ykclient return value (%d): %s", retval, ykclient_strerror(retval)));
	    free(otp);
            otp = NULL;

	    switch (retval) {
	    case YKCLIENT_OK:
		retval = OK;
		break;

	    default:
	        syslog(LOG_INFO, "Yubikey server response: %s (%d)", ykclient_strerror(retval), retval);
		retval = ERROR;
		break;
	    }
	} else {
	    syslog(LOG_INFO, "No user input!");
	    retval = ERROR;
	}
    }

  done:

    if(ykc) ykclient_done(&ykc);

    retval = retval == OK ? PAM_SUCCESS : PAM_AUTH_ERR;
    return retval;
}

int yk_load_user_file(module_config *cfg, const char *username, char ***user_publicids)
{
    int fd, retval;
    ssize_t bytes_read = 0;
    size_t yk_id_pos = 0, yk_id_len = 0;
    char filename[1024], pw_buf[2048], buf[2048];
    char *buf_pos = NULL, *buf_next_line = NULL;
    struct passwd pw_entry, *user_entry;
    struct stat st;
    char **yk_publicids = NULL;
    size_t buf_len = 0, buf_remaining_len = 0;

    getpwnam_r(username, &pw_entry, pw_buf, 2048, &user_entry);
    if(!user_entry) {
        syslog(LOG_ERR, "Can't get passwd entry for '%s'", username);
        return ERROR;
    }

    snprintf(filename, 1024, "%s/%s", user_entry->pw_dir, cfg->yk_user_file);

    retval = stat(filename, &st);
    if(retval < 0) {
        syslog(LOG_ERR, "Can't get stats of file '%s'", filename);
        return ERROR;
    }

    if(!S_ISREG(st.st_mode)) {
        syslog(LOG_ERR, "Not a regular file '%s'", filename);
        return ERROR;
    }

    fd = open(filename, O_RDONLY);
    if(fd < 0) {
        syslog(LOG_ERR, "Can't open file '%s'", filename);
        return ERROR;
    }

    buf_pos = buf;
    buf_len = sizeof(buf) / sizeof(char);
    buf_remaining_len = 0;

    while((bytes_read = read(fd, buf_pos, buf_len - buf_remaining_len)) > 0) {
        buf[bytes_read] = 0;
        buf_pos = buf;

        while((buf_next_line = strchr(buf_pos, '\n'))) {
            *(buf_next_line++) = 0;
            retval = yk_get_publicid(buf_pos, &yk_id_pos, &yk_id_len, &yk_publicids);
            if(retval != OK) {
                yk_free_publicids(yk_publicids);
                return ERROR;
            }

            buf_pos = buf_next_line;
        }

        buf_len = strlen(buf_pos);
        memmove(buf, buf_pos, buf_len + 1);
        buf_pos = buf + buf_len;
    }

    if(buf_len) {
        retval = yk_get_publicid(buf_pos, &yk_id_pos, &yk_id_len, &yk_publicids);
        if(retval != OK) {
            yk_free_publicids(yk_publicids);
            return ERROR;
        }
    }

    *user_publicids = yk_publicids;
    return OK;
}

void yk_free_publicids(char **publicids)
{
    if(publicids) {
        char **yk_id_p;
        for(yk_id_p = publicids; *yk_id_p; ++yk_id_p)
            free(*yk_id_p);

        free(publicids);
    }
}

int yk_get_publicid(char *buf, size_t *yk_id_pos, size_t *yk_id_len, char ***yk_publicids)
{
    if(buf[0] != '#') {
        if(strlen(buf) >= YK_PUBLICID_LEN &&
            (buf[YK_PUBLICID_LEN] == 0    ||
             buf[YK_PUBLICID_LEN] == '\r' ||
             buf[YK_PUBLICID_LEN] == ' '  ||
             buf[YK_PUBLICID_LEN] == '\t' ||
             buf[YK_PUBLICID_LEN] == '#')) {

            if (!*yk_id_len || *yk_id_pos == *yk_id_len - 1) {
                *yk_id_len += YK_IDS_DEFAULT_SIZE;
                *yk_publicids = (char **) realloc (*yk_publicids, *yk_id_len * sizeof(char *));
                if (!*yk_publicids) {
                    return ERROR;
                }
            }
                    
            (*yk_publicids)[*yk_id_pos] = (char *) calloc(YK_PUBLICID_LEN + 1, sizeof(char));
            if (!(*yk_publicids)[*yk_id_pos]) {
                return ERROR;
            }

            buf[YK_PUBLICID_LEN] = 0;
            strncpy((*yk_publicids)[*yk_id_pos], buf, YK_PUBLICID_LEN + 1);
            (*yk_publicids)[++*yk_id_pos] = NULL;

        } else {
            syslog(LOG_WARNING, "Invalid yubikey public id: %s", buf);
        }
    }

    return OK;   
}
