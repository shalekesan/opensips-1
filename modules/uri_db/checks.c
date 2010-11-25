/*
 * $Id$
 *
 * Various URI checks
 *
 * Copyright (C) 2001-2004 FhG FOKUS
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 * 2003-02-26: Created by janakj
 * 2004-03-20: has_totag introduced (jiri)
 * 2004-06-07  updated to the new DB api, added uridb_db_{bind,init,close,ver}
 *              (andrei)
 * 2008-11-07: Added statistics to module: positive_checks and negative_checks (saguti)
 */

#include <string.h>
#include "../../str.h"
#include "../../dprint.h"               /* Debugging */
#include "../../parser/digest/digest.h" /* get_authorized_cred */
#include "../../parser/parse_from.h"
#include "../../parser/parse_uri.h"
#include "../../ut.h"                   /* Handy utilities */
#include "../../db/db.h"                /* Database API */
#include "uridb_mod.h"
#include "checks.h"

static db_con_t* db_handle = NULL;   /* Database connection handle */
static db_func_t uridb_dbf;

/* Return codes reference */
#define OK 		 	 1		/* success */
#define ERR_INTERNAL		-1		/* Internal Error */
#define ERR_CREDENTIALS 	-2		/* No credentials error */
#define ERR_DBUSE  		-3		/* Data Base Use error */
#define ERR_USERNOTFOUND  	-4		/* No found username error */
#define ERR_DBEMTPYRES		-5		/* Emtpy Query Result */

#define ERR_DBACCESS	   	-7     		/* Data Base Access Error */
#define ERR_DBQUERY	  	-8		/* Data Base Query Error */
#define ERR_SPOOFEDUSER   	-9		/* Spoofed User Error */
#define ERR_NOMATCH	    	-10		/* No match Error */


/*
 * Check if a header field contains the same username
 * as digest credentials
 */
static inline int check_username(struct sip_msg* _m, struct sip_uri *_uri)
{
	static db_ps_t my_ps = NULL;
	struct hdr_field* h;
	auth_body_t* c;
	db_key_t keys[3];
	db_val_t vals[3];
	db_key_t cols[1];
	db_res_t* res = NULL;

	if (_uri == NULL) {
		LM_ERR("Bad parameter\n");
		return ERR_INTERNAL;
	}

	/* Get authorized digest credentials */
	get_authorized_cred(_m->authorization, &h);
	if (h == NULL) {
		get_authorized_cred(_m->proxy_auth, &h);
		if (h == NULL) {
			LM_ERR("No authorized credentials found (error in scripts)\n");
			LM_ERR("Call {www,proxy}_authorize before calling check_* functions!\n");
			update_stat(negative_checks, 1);
			return ERR_CREDENTIALS;
		}
	}

	c = (auth_body_t*)(h->parsed);

	/* Parse To/From URI */
	/* Make sure that the URI contains username */
	if (_uri->user.len == 0) {
		LM_ERR("Username not found in URI\n");
		return ERR_USERNOTFOUND;
	}

	/* If use_uri_table is set, use URI table to determine if Digest username
	 * and To/From username match. URI table is a table enumerating all allowed
	 * usernames for a single, thus a user can have several different usernames
	 * (which are different from digest username and it will still match)
	 */
	if (use_uri_table != 0) {
		keys[0] = &uridb_user_col;
		keys[1] = &uridb_domain_col;
		keys[2] = &uridb_uriuser_col;
		cols[0] = &uridb_user_col;

		/* The whole fields are type DB_STR, and not null */
		VAL_TYPE(vals) = VAL_TYPE(vals + 1) = VAL_TYPE(vals + 2) = DB_STR;
		VAL_NULL(vals) = VAL_NULL(vals + 1) = VAL_NULL(vals + 2) = 0;

		VAL_STR(vals) = c->digest.username.user;
		VAL_STR(vals + 1) = *GET_REALM(&c->digest);
		VAL_STR(vals + 2) = _uri->user;

		uridb_dbf.use_table(db_handle, &db_table);
		CON_PS_REFERENCE(db_handle) = &my_ps;

		if (uridb_dbf.query(db_handle, keys, 0, vals, cols, 3, 1, 0, &res) < 0)
		{
			LM_ERR("Error while querying database\n");
			return ERR_DBQUERY;
		}

		/* If the previous function returns at least one row, it means
		 * there is an entry for given digest username and URI username
		 * and thus this combination is allowed and the function will match
		 */
		if (RES_ROW_N(res) == 0) {
			LM_DBG("From/To user '%.*s' is spoofed\n",
				   _uri->user.len, ZSW(_uri->user.s));
			uridb_dbf.free_result(db_handle, res);
			update_stat(negative_checks, 1);
			return ERR_SPOOFEDUSER;
		} else {
			LM_DBG("From/To user '%.*s' and auth user match\n",
				   _uri->user.len, ZSW(_uri->user.s));
			uridb_dbf.free_result(db_handle, res);
			update_stat(positive_checks, 1);
			return OK;
		}
	} else {
		/* URI table not used, simply compare digest username and From/To
		 * username, the comparison is case insensitive
		 */
		if (_uri->user.len == c->digest.username.user.len) {
			if (!strncasecmp(_uri->user.s, c->digest.username.user.s, 
			_uri->user.len)) {
				LM_DBG("Digest username and URI username match\n");
				update_stat(positive_checks, 1);
				return OK;
			}
		}

		LM_DBG("Digest username and URI username do NOT match\n");
		update_stat(negative_checks, 1);
		return ERR_NOMATCH;
	}
}


/*
 * Check username part in To header field
 */
int check_to(struct sip_msg* _m, char* _s1, char* _s2)
{
	if (!_m->to && ((parse_headers(_m, HDR_TO_F, 0) == -1) || (!_m->to))) {
		LM_ERR("Error while parsing To header field\n");
		return ERR_INTERNAL;
	}
	if(parse_to_uri(_m)==NULL) {
		LM_ERR("Error while parsing To header URI\n");
		return ERR_INTERNAL;
	}

	return check_username(_m, &get_to(_m)->parsed_uri);
}


/*
 * Check username part in From header field
 */
int check_from(struct sip_msg* _m, char* _s1, char* _s2)
{
	if (parse_from_header(_m) < 0) {
		LM_ERR("Error while parsing From header field\n");
		return ERR_INTERNAL;
	}
	if(parse_from_uri(_m)==NULL) {
		LM_ERR("Error while parsing From header URI\n");
		return ERR_INTERNAL;
	}

	return check_username(_m, &get_from(_m)->parsed_uri);
}


/*
 * Check if uri belongs to a local user
 */
int does_uri_exist(struct sip_msg* _msg, char* _s1, char* _s2)
{
	static db_ps_t my_ps = NULL;
	db_key_t keys[2];
	db_val_t vals[2];
	db_key_t cols[1];
	db_res_t* res = NULL;

	if (parse_sip_msg_uri(_msg) < 0) {
		LM_ERR("Error while parsing URI\n");
		return ERR_INTERNAL;
	}

	if (use_uri_table != 0) {
		uridb_dbf.use_table(db_handle, &db_table);
		keys[0] = &uridb_uriuser_col;
		keys[1] = &uridb_domain_col;
		cols[0] = &uridb_uriuser_col;
	} else {
		uridb_dbf.use_table(db_handle, &db_table);
		keys[0] = &uridb_user_col;
		keys[1] = &uridb_domain_col;
		cols[0] = &uridb_user_col;
	}

	VAL_TYPE(vals) = VAL_TYPE(vals + 1) = DB_STR;
	VAL_NULL(vals) = VAL_NULL(vals + 1) = 0;
	VAL_STR(vals) = _msg->parsed_uri.user;
	VAL_STR(vals + 1) = _msg->parsed_uri.host;

	CON_PS_REFERENCE(db_handle) = &my_ps;

	if (uridb_dbf.query(db_handle, keys, 0, vals, cols, (use_domain ? 2 : 1),
				1, 0, &res) < 0) {
		LM_ERR("Error while querying database\n");
		return ERR_USERNOTFOUND;
	}
	
	if (RES_ROW_N(res) == 0) {
		LM_DBG("User in request uri does not exist\n");
		uridb_dbf.free_result(db_handle, res);
		return ERR_DBEMTPYRES;
	} else {
		LM_DBG("User in request uri does exist\n");
		uridb_dbf.free_result(db_handle, res);
		return OK;
	}
}



int uridb_db_init(const str* db_url)
{
	if (uridb_dbf.init==0){
		LM_CRIT("BUG: null dbf\n");
		return -1;
	}
	
	db_handle=uridb_dbf.init(db_url);
	if (db_handle==NULL){
		LM_ERR("unable to connect to the database\n");
		return -1;
	}
	return 0;
}



int uridb_db_bind(const str* db_url)
{
	if (db_bind_mod(db_url, &uridb_dbf)<0){
		LM_ERR("unable to bind to the database module\n");
		return -1;
	}

	if (!DB_CAPABILITY(uridb_dbf, DB_CAP_QUERY)) {
		LM_ERR("Database module does not implement the 'query' function\n");
		return -1;
	}

	return 0;
}


void uridb_db_close(void)
{
	if (db_handle != NULL && uridb_dbf.close != 0){
		uridb_dbf.close(db_handle);
		db_handle=NULL;
	}
}