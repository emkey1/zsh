/* Generated automatically */
static char*convertattr(char*attrstr,int sgr);
static HashNode getgroup(const char*name,int sgr);
static void scangroup(ScanFunc func,int flags,int sgr);
static HashNode getpmesc(UNUSED(HashTable ht),const char*name);
static void scanpmesc(UNUSED(HashTable ht),ScanFunc func,int flags);
static HashNode getpmsgr(UNUSED(HashTable ht),const char*name);
static void scanpmsgr(UNUSED(HashTable ht),ScanFunc func,int flags);
