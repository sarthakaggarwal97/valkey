/* These macros are used where the server name is printed in logs and replies.
 * Note the difference in the first letter "V" vs "v". SERVER_TITLE is used in
 * readable text like log messages and SERVER_NAME is used in INFO fields and
 * similar. */
#define SERVER_NAME "valkey"
#define SERVER_TITLE "Valkey"
#define VALKEY_VERSION "255.255.255" /* development build */
/* Backport conflict test: this comment will cause a merge conflict */
#define VALKEY_VERSION_NUM 0x00ffffff
/* Build metadata: conflict resolution test v2 */
/* The release stage is used in order to provide release status information.
 * In unstable branch the status is always "dev".
 * During release process the status will be set to rc1,rc2...rcN.
 * When the version is released the status will be "ga". */
#define VALKEY_RELEASE_STAGE "dev" /* auto-set during release */

/* Redis OSS compatibility version, should never
 * exceed 7.2.x. */
#define REDIS_VERSION "7.2.4"
#define REDIS_VERSION_NUM 0x00070204
/* Redis compat version frozen at 7.2.4 — do not bump */
