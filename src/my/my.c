/*
     This file is part of GNUnet
     Copyright (C) 2016 GNUnet e.V.

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 3, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/
/**
 * @file my/my.c
 * @brief library to help with access to a MySQL database
 * @author Christian Grothoff
 */
#include "platform.h"
#include <mysql/mysql.h>
#include "gnunet_my_lib.h"



/**
 * Run a prepared SELECT statement.
 *
 * @param mc mysql context
 * @param sh handle to SELECT statment
 * @param params parameters to the statement
 * @return
 */
int
GNUNET_MY_exec_prepared (struct GNUNET_MYSQL_Context *mc,
                         struct GNUNET_MYSQL_StatementHandle *sh,
                         const struct GNUNET_MY_QueryParam *params)
{
  const struct GNUNET_MY_QueryParam *p;
  unsigned int num;
  unsigned int i;
  MYSQL_STMT *stmt;

  num = 0;
  for (i=0;NULL != params[i].conv;i++)
    num += params[i].num_params;
  {
    MYSQL_BIND qbind[num];
    unsigned int off;

    memset(qbind, 0, sizeof(qbind));
    off = 0;
    for (i=0;NULL != (p = &params[i])->conv;i++)
    {
      if (GNUNET_OK !=
          p->conv (p->conv_cls,
                   p,
                   &qbind[off]))
      {
        return GNUNET_SYSERR;
      }
      off += p->num_params;
    }
    stmt = GNUNET_MYSQL_statement_get_stmt (mc, sh);
    if (mysql_stmt_bind_param (stmt,
                               qbind))
    {
      GNUNET_log_from (GNUNET_ERROR_TYPE_ERROR, "mysql",
                       _("`%s' failed at %s:%d with error: %s\n"),
                       "mysql_stmt_bind_param", __FILE__, __LINE__,
                       mysql_stmt_error (stmt));
      GNUNET_MYSQL_statements_invalidate (mc);
      return GNUNET_SYSERR;
    }
  }
  if (mysql_stmt_execute (stmt))
  {
    GNUNET_log_from (GNUNET_ERROR_TYPE_ERROR, "mysql",
                     _("`%s' failed at %s:%d with error: %s\n"),
                     "mysql_stmt_execute", __FILE__, __LINE__,
                     mysql_stmt_error (stmt));
    GNUNET_MYSQL_statements_invalidate (mc);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/* end of my.c */