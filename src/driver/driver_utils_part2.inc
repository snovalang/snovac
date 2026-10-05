int sn_driver_path_is_manifest(const char *path) {
  if (!path) {
    return 0;
  }
  const char *base = path_basename(path);
  return strcmp(base, "mod.sns") == 0 || strcmp(base, "snova.sns") == 0;
}

int sn_driver_path_is_script(const char *path) {
  if (!path || sn_driver_path_is_manifest(path)) {
    return 0;
  }
  return ends_with_exact(path_basename(path), ".sns");
}

/* Recursively creates `dir` and any missing parent directories (mkdir -p). */
static int mkdir_p(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0 ||
      strcmp(dir, "\\") == 0) {
    return 1;
  }
#if defined(_WIN32)
  if (strlen(dir) == 2 && dir[1] == ':') {
    return 1;
  }
  if (strlen(dir) == 3 && dir[1] == ':' && (dir[2] == '/' || dir[2] == '\\')) {
    return 1;
  }
#endif
  struct stat st;
  if (stat(dir, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }

  char parent[SNOVAC_PATH_MAX];
  sn_driver_dirname_into(dir, parent, sizeof(parent));
  if (strcmp(parent, dir) != 0) {
    if (!mkdir_p(parent)) {
      return 0;
    }
  }

#if defined(_WIN32)
  if (_mkdir(dir) != 0 && errno != EEXIST) {
    return 0;
  }
#else
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return 0;
  }
#endif
  return 1;
}

/* Ensures the parent directory of `path` exists, creating it (and any
 * missing ancestors) if necessary. Used before writing output/temporary
 * build artifacts so callers don't have to pre-create -o's directory. */
void sn_driver_ensure_parent_dir(const char *path) {
  char dir[SNOVAC_PATH_MAX];
  sn_driver_dirname_into(path, dir, sizeof(dir));
  mkdir_p(dir);
}

void sn_driver_normalize_path(const char *path, char *out, size_t out_sz) {
  if (!path || !path[0]) {
    if (out && out_sz > 0) out[0] = '\0';
    return;
  }
#if defined(_WIN32)
  char resolved[SNOVAC_PATH_MAX];
  const char *src = path;
  char msys_buf[SNOVAC_PATH_MAX];
  /* Handle MSYS/Git-Bash/Cygwin absolute paths like "/c/Users/..." -> "C:/Users/..." */
  if (path[0] == '/' && isalpha((unsigned char)path[1]) && (path[2] == '/' || path[2] == '\0')) {
    snprintf(msys_buf, sizeof(msys_buf), "%c:%s", (char)toupper((unsigned char)path[1]), path + 2);
    src = msys_buf;
  }
  if (_fullpath(resolved, src, sizeof(resolved)) != NULL) {
    for (char *p = resolved; *p; p++) {
      if (*p == '\\') {
        *p = '/';
      }
    }
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", src);
  }
#else
  /* glibc fortify requires the realpath buffer to be at least PATH_MAX. */
  char resolved[PATH_MAX];
  if (realpath(path, resolved) != NULL) {
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", path);
  }
#endif
}

int sn_driver_execv(char *const argv[]) {
  if (!argv || !argv[0] || !argv[0][0]) {
    return -1;
  }
#if defined(_WIN32)
  intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  if (rc < 0) {
    return -1;
  }
  return (int)rc;
#else
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
#endif
}

#if defined(_WIN32)
/* CreateProcess takes one command line. Quote every argument and refuse a
 * quote character so the line cannot be re-split. This is not a shell. */
static int append_quoted_arg(char *dst, size_t cap, size_t *used, const char *arg) {
  size_t n = strlen(arg);
  if (memchr(arg, '"', n) != NULL) {
    return 0;
  }
  if (*used + n + 3 >= cap) {
    return 0;
  }
  if (*used > 0) {
    dst[(*used)++] = ' ';
  }
  dst[(*used)++] = '"';
  memcpy(dst + *used, arg, n);
  *used += n;
  dst[(*used)++] = '"';
  dst[*used] = '\0';
  return 1;
}
#endif

int sn_driver_execv_read(char *const argv[], char *out, size_t out_sz) {
  if (out && out_sz > 0) {
    out[0] = '\0';
  }
  if (!argv || !argv[0] || !argv[0][0]) {
    return -1;
  }
#if defined(_WIN32)
  char cmdline[8192];
  size_t used = 0;
  cmdline[0] = '\0';
  for (int i = 0; argv[i]; i++) {
    if (!append_quoted_arg(cmdline, sizeof(cmdline), &used, argv[i])) {
      return -1;
    }
  }

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;
  HANDLE rd = NULL;
  HANDLE wr = NULL;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    return -1;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = wr;
  si.hStdError = (nul != INVALID_HANDLE_VALUE) ? nul : GetStdHandle(STD_ERROR_HANDLE);

  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  CloseHandle(wr);
  if (nul != INVALID_HANDLE_VALUE) {
    CloseHandle(nul);
  }
  if (!ok) {
    CloseHandle(rd);
    return -1;
  }

  size_t n = 0;
  for (;;) {
    char sink[512];
    DWORD got = 0;
    char *dest = sink;
    DWORD want = (DWORD)sizeof(sink);
    if (out && out_sz > 1 && n + 1 < out_sz) {
      dest = out + n;
      want = (DWORD)(out_sz - 1 - n);
      if (want > 512) {
        want = 512;
      }
    }
    if (!ReadFile(rd, dest, want, &got, NULL) || got == 0) {
      break;
    }
    if (dest != sink) {
      n += (size_t)got;
    }
  }
  if (out && out_sz > 0) {
    if (n >= out_sz) {
      n = out_sz - 1;
    }
    out[n] = '\0';
  }
  CloseHandle(rd);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)code;
#else
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    close(fds[1]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  close(fds[1]);
  {
    size_t n = 0;
    char sink[256];
    for (;;) {
      char *dest = sink;
      size_t want = sizeof(sink);
      if (out && out_sz > 1 && n + 1 < out_sz) {
        dest = out + n;
        want = out_sz - 1 - n;
      }
      ssize_t r = read(fds[0], dest, want);
      if (r <= 0) {
        break;
      }
      if (dest != sink) {
        n += (size_t)r;
      }
    }
    if (out && out_sz > 0) {
      if (n >= out_sz) {
        n = out_sz - 1;
      }
      out[n] = '\0';
    }
  }
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
#endif
}
