/*
 * src/query.c
 * Utility to query and display stored POST data from the ndbm database.
 */

#include <fcntl.h>
#include <ndbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Constants --- */
enum QueryConstants { DB_FILENAME_SIZE = 64, DB_PERMS = 0666 };

int main(void) {
  DBM *db = NULL;
  char db_filename[DB_FILENAME_SIZE];
  datum key;

  /* Populate at runtime to satisfy cppcheck and strict GCC const rules */
  (void)strncpy(db_filename, "post_database", sizeof(db_filename) - 1);
  db_filename[sizeof(db_filename) - 1] = '\0';

  db = dbm_open(db_filename, O_RDONLY, DB_PERMS);
  if (db == NULL) {
    (void)fprintf(stderr,
                  "Error: Could not open database '%s'. Make sure data has "
                  "been POSTed.\n",
                  db_filename);
    return EXIT_FAILURE;
  }

  (void)printf("--- Stored POST Data ---\n");

/* Disable the aggregate return warning specifically for the ndbm API calls */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
#endif

  /* Iterate through all keys in the database */
  for (key = dbm_firstkey(db); key.dptr != NULL; key = dbm_nextkey(db)) {
    /* Declared at the top of the block to satisfy C89, scoped tight for
     * cppcheck */
    datum value;

    value = dbm_fetch(db, key);
    if (value.dptr != NULL) {
      /* Use %.*s to safely print datum strings, as they are not guaranteed to
       * be null-terminated */
      (void)printf("Key: %.*s | Value: %.*s\n", key.dsize, key.dptr,
                   value.dsize, value.dptr);
    } else {
      (void)printf("Key: %.*s | Value: (null)\n", key.dsize, key.dptr);
    }
  }

/* Restore the strict compiler warnings */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

  (void)printf("------------------------\n");

  dbm_close(db);
  return EXIT_SUCCESS;
}
