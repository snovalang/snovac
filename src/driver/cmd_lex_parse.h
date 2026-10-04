/* cmd_lex_parse.h — lexing, parsing, and token/ast dumping commands. */
#ifndef SNOVAC_CMD_LEX_PARSE_H
#define SNOVAC_CMD_LEX_PARSE_H

#include "token.h"
#include "lex.h"

void sn_cmd_dump_tokens(const SnTokenVec *toks);
int sn_cmd_lex(const char *path, int dump);
int sn_cmd_parse(const char *path, int dump);

#endif /* SNOVAC_CMD_LEX_PARSE_H */
