#include "common.h"
#include "crossplatform.h"

// Codes compatible with Windows and Linux
#ifndef _WIN32

// For internal use
// wMilliseconds is not needed
void tmToSystemTime(const tm *tm, SYSTEMTIME *out) {
    out->wYear = tm->tm_year + 1900;
    out->wMonth = tm->tm_mon + 1;
    out->wDayOfWeek = tm->tm_wday;
    out->wDay = tm->tm_mday;
    out->wHour = tm->tm_hour;
    out->wMinute = tm->tm_min;
    out->wSecond = tm->tm_sec;
}

void GetLocalTime_CP(SYSTEMTIME *out) {
    time_t timestamp = time(nil);
    tm *localTm = localtime(&timestamp);
    tmToSystemTime(localTm, out);
}
#endif

// Compatible with Linux/POSIX and MinGW on Windows
#ifndef _WIN32
#ifdef RW_SOFT
// The console's newlib has no <dirent.h> and no way to enumerate a directory,
// so the only thing that can be resolved here is one exact file name.  A
// wildcard reports "nothing found": the skin list and the memory card browser
// then show just their defaults, which is how the port runs without a skin
// folder or a memory card.
HANDLE FindFirstFile(const char* pathname, WIN32_FIND_DATA* firstfile)
{
	memset(firstfile, 0, sizeof(*firstfile));

	char resolved[512];
	snprintf(resolved, sizeof(resolved), "%s", pathname);
	char *star = strchr(resolved, '*');
	if(star)
		return NULL;	// a pattern: nothing to enumerate with

	for(char *p = resolved; *p; p++)
		if(*p == '\\')
			*p = '/';

	char *real = casepath(resolved, true);
	struct stat st;
	if(real == NULL || stat(real, &st) != 0 || !S_ISREG(st.st_mode)){
		if(real)
			free(real);
		return INVALID_HANDLE_VALUE;
	}

	const char *slash = strrchr(real, '/');
	strncpy(firstfile->folder, slash ? slash + 1 : real, sizeof(firstfile->folder) - 1);
	strncpy(firstfile->cFileName, slash ? slash + 1 : real, sizeof(firstfile->cFileName) - 1);
	firstfile->ftLastWriteTime = st.st_mtime;
	free(real);

	// the sentinel below is never a real HANDLE, so FindNextFile just fails
	return (HANDLE)&firstfile->ftLastWriteTime;
}

bool FindNextFile(HANDLE d, WIN32_FIND_DATA* finddata)
{
	(void)d;
	(void)finddata;
	return false;
}
#else
HANDLE FindFirstFile(const char* pathname, WIN32_FIND_DATA* firstfile) {
	char newpathname[64];
	snprintf(newpathname, sizeof(newpathname), "E:/data/gta3/%s", pathname);
	char* path = strtok(newpathname, "\\*");
	strncpy(firstfile->folder, path, sizeof(firstfile->folder));

	// Both w/ extension and w/o extension is ok
	if (strlen(path) + 2 != strlen(pathname))
		strncpy(firstfile->extension, strtok(NULL, "\\*"), sizeof(firstfile->extension));
	else
		strncpy(firstfile->extension, "", sizeof(firstfile->extension));

	HANDLE d;
	if ((d = (HANDLE)opendir(path)) == NULL || !FindNextFile(d, firstfile))
		return NULL;

	return d;
}

bool FindNextFile(HANDLE d, WIN32_FIND_DATA* finddata) {
	dirent *file;
	static struct stat fileStats;
	static char path[PATH_MAX], relativepath[NAME_MAX + sizeof(finddata->folder) + 1];
	int extensionLen = strlen(finddata->extension);
	while ((file = readdir((DIR*)d)) != NULL) {

		if ((extensionLen == 0 || strncmp(&file->d_name[strlen(file->d_name) - extensionLen], finddata->extension, extensionLen) == 0)) {

			sprintf(relativepath, "%s/%s", finddata->folder, file->d_name);
			// realpath(relativepath, path);
			strcpy(path, relativepath);
			stat(path, &fileStats);
			strncpy(finddata->cFileName, file->d_name, sizeof(finddata->cFileName));
			finddata->ftLastWriteTime = fileStats.st_mtime;
			return true;
		}
	}
	return false;
}
#endif

void GetDateFormat(int unused1, int unused2, SYSTEMTIME* in, int unused3, char* out, int size) {
	tm linuxTime;
	linuxTime.tm_year = in->wYear - 1900;
	linuxTime.tm_mon = in->wMonth - 1;
	linuxTime.tm_wday = in->wDayOfWeek;
	linuxTime.tm_mday = in->wDay;
	linuxTime.tm_hour = in->wHour;
	linuxTime.tm_min = in->wMinute;
	linuxTime.tm_sec = in->wSecond;
	strftime(out, size, nl_langinfo(D_FMT), &linuxTime);
}

void FileTimeToSystemTime(time_t* writeTime, SYSTEMTIME* out) {
	tm *ptm = gmtime(writeTime);
	tmToSystemTime(ptm, out);
}
#endif

// Because wchar length differs between platforms.
wchar*
AllocUnicode(const char* src)
{
	wchar *dst = (wchar*)malloc(strlen(src)*2 + 2);
	wchar *i = dst;
	while((*i++ = (unsigned char)*src++) != '\0');
	return dst;
}

// Funcs/features from Windows that we need on other platforms
#ifndef _WIN32
char *strupr(char *s) {
    char* tmp = s;

    for (;*tmp;++tmp) {
        *tmp = toupper((unsigned char) *tmp);
    }

    return s;
}
char *strlwr(char *s) {
    char* tmp = s;

    for (;*tmp;++tmp) {
        *tmp = tolower((unsigned char) *tmp);
    }

    return s;
}

char *trim(char *s) {
    char *ptr;
    if (!s)
        return NULL;   // handle NULL string
    if (!*s)
        return s;      // handle empty string
    for (ptr = s + strlen(s) - 1; (ptr >= s) && isspace(*ptr); --ptr);
    ptr[1] = '\0';
    return s;
}

FILE* _fcaseopen(char const* filename, char const* mode)
{
    FILE* result;
    char* real = casepath(filename);
    if (!real)
        result = fopen(filename, mode);
    else {
        result = fopen(real, mode);
        free(real);
    }
    return result;
}

int _caserename(const char *old_filename, const char *new_filename)
{
    int result;
    char *real_old = casepath(old_filename);
    char *real_new = casepath(new_filename);

    // hack so we don't even try to rename it to new_filename if it already exists
    if (!real_new) {
        free(real_old);
        return -1;
    }

    if (!real_old)
        result = rename(old_filename, real_new);
    else
        result = rename(real_old, real_new);

    free(real_old);
    free(real_new);

    return result;
}

// Case-insensitivity (from https://github.com/OneSadCookie/fcaseopen)
//
// re3 was written for Windows, so it asks for "MODELS\\FONTS.TXD" while the
// files on disk are "models/fonts_r.txd".  Neither Linux nor the console's FAT
// driver folds case, so each path component is resolved here.  Returned string should be freed manually; NULL means
// "no such file", which the callers already handle by trying the name as-is.
// Returned string should freed manually (if exists)
char* casepath(char const* path, bool checkPathFirst)
{
	(void)checkPathFirst;
	if(path == NULL)
		return NULL;

	// re3 spells every path with Windows separators; translate them first.
	char *norm = strdup(path);
	if(norm == NULL)
		return NULL;
	for(char *p = norm; *p; p++)
		if(*p == '\\')
			*p = '/';

	// strip trailing separators
	size_t nl = strlen(norm);
	while(nl > 1 && (norm[nl-1] == '/' || norm[nl-1] == ' '))
		norm[--nl] = '\0';

	// absolute start directory
	char cwd[512];
	char *work;
	if(norm[0] == '/'){
		work = strdup("/");
	}else if(strncmp(norm, "E:", 2) == 0 || strncmp(norm, "F:", 2) == 0){
		// drive letters are a PS2-ism; nothing to resolve
		return norm;
	}else{
		if(getcwd(cwd, sizeof(cwd)) == NULL)
			cwd[0] = '\0';
		work = (char*)malloc(strlen(cwd) + 2);
		sprintf(work, "%s/", cwd);
	}

	// Every path is assembled with a trailing separator: appending a component
	// to a prefix that has one can never glue two path elements together.
	char *cur = work;
	char *p = norm;
	while(*p){
		while(*p == '/')
			p++;
		if(!*p)
			break;
		char *comp = p;
		while(*p && *p != '/')
			p++;
		size_t clen = p - comp;
		// "." and ".." are fine as-is
		if(clen == 1 && comp[0] == '.'){
			continue;
		}
		if(clen == 2 && comp[0] == '.' && comp[1] == '.'){
			char *slash = strrchr(cur, '/');
			if(slash && slash != cur)
				*slash = '\0';
			continue;
		}

		char *joined = (char*)malloc(strlen(cur) + clen + 2);
		sprintf(joined, "%s%.*s/", cur, (int)clen, comp);
		struct stat st;
		if(stat(joined, &st) == 0){
			free(cur);
			cur = joined;
			continue;
		}

		/*
		 * Not there under that name.  The console's newlib has no <dirent.h>,
		 * so instead of walking the directory this probes the spellings that
		 * actually occur in GTA III's data files: the component upper-cased and
		 * lower-cased.  That covers the console's FAT volumes (whose names are
		 * upper case) as well as a case sensitive host filesystem.
		 */
		char *found = NULL;
		for(int variant = 0; variant < 2 && found == NULL; variant++){
			char *cand = (char*)malloc(strlen(cur) + clen + 2);
			sprintf(cand, "%s%.*s", cur, (int)clen, comp);
			for(size_t i = 0; i < clen; i++){
				if(variant == 0)
					cand[strlen(cur) + i] = (char)toupper((unsigned char)cand[strlen(cur) + i]);
				else
					cand[strlen(cur) + i] = (char)tolower((unsigned char)cand[strlen(cur) + i]);
			}
			/* Probe without the trailing separator - stat() rejects one on a
			 * regular file - and add it only to the spelling that is kept. */
			struct stat st;
			if(stat(cand, &st) == 0){
				char *withSlash = (char*)malloc(strlen(cand) + 2);
				sprintf(withSlash, "%s/", cand);
				free(cand);
				found = withSlash;
			}else
				free(cand);
		}
		if(found == NULL){
			/* Tracing is opt-in: on a device there is no debugger to attach. */
			if(getenv("RE3_TRACE_FS"))
				fprintf(stderr, "casepath: '%s' has no component '%.*s' under '%s'\n", path, (int)clen, comp, cur);
			// Nothing matched.  Hand back the path as far as it was resolved, in
			// the normalised spelling, so the caller's open() reports the missing
			// file instead of failing on a stray backslash.
			free(joined);
			free(cur);
			free(norm);
			return strdup(path);
		}
		free(cur);
		free(joined);
		cur = found;
	}
	free(norm);
	// hand back a plain path without the trailing separator
	size_t ol = strlen(cur);
	while(ol > 1 && cur[ol-1] == '/')
		cur[--ol] = '\0';
	return cur;
}
#endif

#ifdef __SWITCH__
/* Taken from glibc */
char *realpath(const char *name, char *resolved)
{
   char *rpath, *dest = NULL;
   const char *start, *end, *rpath_limit;
   long int path_max;

   /* As per Single Unix Specification V2 we must return an error if
      either parameter is a null pointer.  We extend this to allow
      the RESOLVED parameter to be NULL in case the we are expected to
      allocate the room for the return value.  */
   if (!name)
      return NULL;

   /* As per Single Unix Specification V2 we must return an error if
      the name argument points to an empty string.  */
   if (name[0] == '\0')
      return NULL;

#ifdef PATH_MAX
   path_max = PATH_MAX;
#else
   path_max = pathconf(name, _PC_PATH_MAX);
   if (path_max <= 0)
      path_max = 1024;
#endif

   if (!resolved)
   {
      rpath = (char*)malloc(path_max);
      if (!rpath)
         return NULL;
   }
   else
      rpath = resolved;
   rpath_limit = rpath + path_max;

   if (name[0] != '/')
   {
      if (!getcwd(rpath, path_max))
      {
         rpath[0] = '\0';
         goto error;
      }
      dest = (char*)memchr(rpath, '\0', path_max);
   }
   else
   {
      rpath[0] = '/';
      dest = rpath + 1;
   }

   for (start = end = name; *start; start = end)
   {
      /* Skip sequence of multiple path-separators.  */
      while (*start == '/')
         ++start;

      /* Find end of path component.  */
      for (end = start; *end && *end != '/'; ++end)
         /* Nothing.  */;

      if (end - start == 0)
         break;
      else if (end - start == 1 && start[0] == '.')
         /* nothing */;
      else if (end - start == 2 && start[0] == '.' && start[1] == '.')
      {
         /* Back up to previous component, ignore if at root already.  */
         if (dest > rpath + 1)
            while ((--dest)[-1] != '/')
               ;
      }
      else
      {
         size_t new_size;

         if (dest[-1] != '/')
            *dest++ = '/';

         if (dest + (end - start) >= rpath_limit)
         {
            ptrdiff_t dest_offset = dest - rpath;
            char *new_rpath;

            if (resolved)
            {
               if (dest > rpath + 1)
                  dest--;
               *dest = '\0';
               goto error;
            }
            new_size = rpath_limit - rpath;
            if (end - start + 1 > path_max)
               new_size += end - start + 1;
            else
               new_size += path_max;
            new_rpath = (char *)realloc(rpath, new_size);
            if (!new_rpath)
               goto error;
            rpath = new_rpath;
            rpath_limit = rpath + new_size;

            dest = rpath + dest_offset;
         }

         dest = (char*)memcpy(dest, start, end - start);
         *dest = '\0';
      }
   }
   if (dest > rpath + 1 && dest[-1] == '/')
      --dest;
   *dest = '\0';

   return rpath;

error:
   if (!resolved)
      free(rpath);
   return NULL;
}

ssize_t readlink (const char * __path, char * __buf, size_t __buflen)
{
   errno = ENOSYS;
   return -1;
}
#endif
