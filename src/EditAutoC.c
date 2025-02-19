// Edit AutoCompletion

#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include "SciCall.h"
#include "VectorISA.h"
#include "Helpers.h"
#include "Edit.h"
#include "Styles.h"
#include "resource.h"
#include "EditAutoC_Data0.h"
#include "LaTeXInput.h"

#define NP2_AUTOC_USE_STRING_ORDER	1
// scintilla/src/AutoComplete.h AutoComplete::maxItemLen
#define NP2_AUTOC_MAX_WORD_LENGTH	(1024 - 3 - 1 - 16)	// SP + '(' + ')' + '\0'
#define NP2_AUTOC_INIT_BUF_SIZE		(4096)
#define NP2_AUTOC_MAX_BUF_COUNT		20
#define NP2_AUTOC_INIT_CACHE_BYTES	(4096)
#define NP2_AUTOC_MAX_CACHE_COUNT	18
/*
word buffer:
(2**20 - 1)*4096 => 4 GiB

node cache:
a = [4096*2**i for i in range(18)] => 1 GiB
x64: sum(i//40 for i in a) => 26843434 nodes
x86: sum(i//24 for i in a) => 44739063 nodes
*/

struct WordNode;
struct WordList {
	char wordBuf[1024];
	int (__cdecl *WL_strcmp)(LPCSTR, LPCSTR);
	int (__cdecl *WL_strncmp)(LPCSTR, LPCSTR, size_t);
#if NP2_AUTOC_USE_STRING_ORDER
	uint32_t (*WL_OrderFunc)(const void *, uint32_t);
#endif
	struct WordNode *pListHead;
	LPCSTR pWordStart;

	char *bufferList[NP2_AUTOC_MAX_BUF_COUNT];
	char *buffer;
	int bufferCount;
	UINT offset;
	UINT capacity;

	UINT nWordCount;
	UINT nTotalLen;
	UINT orderStart;
	int iStartLen;

	struct WordNode *nodeCacheList[NP2_AUTOC_MAX_CACHE_COUNT];
	struct WordNode *nodeCache;
	int cacheCount;
	UINT cacheIndex;
	UINT cacheCapacity;
	UINT cacheBytes;
};

// TODO: replace _stricmp() and _strnicmp() with other functions
// which correctly case insensitively compares UTF-8 string and ANSI string.

#if NP2_AUTOC_USE_STRING_ORDER
#define NP2_AUTOC_ORDER_LENGTH	4

uint32_t WordList_Order(const void *pWord, uint32_t len) {
#if 0
	uint32_t high = 0;
	const uint8_t *ptr = (const uint8_t *)pWord;
	len = min_u(len, NP2_AUTOC_ORDER_LENGTH);
	for (uint32_t i = 0; i < len; i++) {
		high = (high << 8) | *ptr++;
	}
	if (len < NP2_AUTOC_ORDER_LENGTH) {
		high <<= (NP2_AUTOC_ORDER_LENGTH - len)*8;
	}

#else
	uint32_t high = loadle_u32(pWord);
	if (len < NP2_AUTOC_ORDER_LENGTH) {
		high = bit_zero_high_u32(high, len*8);
	}
	high = bswap32(high);
#endif
	return high;
}

uint32_t WordList_OrderCase(const void *pWord, uint32_t len) {
#if 1
	uint32_t high = 0;
	const uint8_t *ptr = (const uint8_t *)pWord;
	len = min_u(len, NP2_AUTOC_ORDER_LENGTH);
	for (uint32_t i = 0; i < len; i++) {
		uint8_t ch = *ptr++;
		// convert to lower case to match _stricmp() / strcasecmp().
		if (ch >= 'A' && ch <= 'Z') {
			ch = ch + 'a' - 'A';
		}
		high = (high << 8) | ch;
	}
	if (len < NP2_AUTOC_ORDER_LENGTH) {
		high <<= (NP2_AUTOC_ORDER_LENGTH - len)*8;
	}

#else
	uint32_t high = loadle_u32(pWord);
	high |= 0x20202020U; // only works for ASCII letters
	if (len < NP2_AUTOC_ORDER_LENGTH) {
		high = bit_zero_high_u32(high, len*8);
	}
	high = bswap32(high);
#endif
	return high;
}
#endif

// Tree
struct WordNode {
	union {
		struct WordNode *link[2];
		struct {
			struct WordNode *left;
			struct WordNode *right;
		};
	};
	char *word;
#if NP2_AUTOC_USE_STRING_ORDER
	UINT order;
#endif
	int len;
	int level;
};

#define NP2_TREE_HEIGHT_LIMIT	32
// TODO: since the tree is sorted, nodes greater than some level can be deleted to reduce total words.
// or only limit word count in WordList_GetList().

// Andersson Tree, source from https://www.eternallyconfuzzled.com/tuts/datastructures/jsw_tut_andersson.aspx
// see also https://en.wikipedia.org/wiki/AA_tree
#define aa_tree_skew(t) \
	if ((t)->level && (t)->left && (t)->level == (t)->left->level) {\
		struct WordNode *save = (t)->left;					\
		(t)->left = save->right;							\
		save->right = (t);									\
		(t) = save;											\
	}
#define aa_tree_split(t) \
	if ((t)->level && (t)->right && (t)->right->right && (t)->level == (t)->right->right->level) {\
		struct WordNode *save = (t)->right;					\
		(t)->right = save->left;							\
		save->left = (t);									\
		(t) = save;											\
		++(t)->level;										\
	}

static inline void WordList_AddBuffer(struct WordList *pWList) {
	char *buffer = (char *)NP2HeapAlloc(pWList->capacity);
	pWList->bufferList[pWList->bufferCount] = buffer;
	pWList->buffer = buffer;
	pWList->bufferCount++;
	pWList->offset = 0;
}

static inline void WordList_AddCache(struct WordList *pWList) {
	struct WordNode *node = (struct WordNode *)NP2HeapAlloc(pWList->cacheBytes);
	pWList->nodeCacheList[pWList->cacheCount] = node;
	pWList->nodeCache = node;
	pWList->cacheCount++;
	pWList->cacheIndex = 0;
	pWList->cacheCapacity = pWList->cacheBytes / (sizeof(struct WordNode));
}

void WordList_AddWord(struct WordList *pWList, LPCSTR pWord, int len) {
	struct WordNode *root = pWList->pListHead;
#if NP2_AUTOC_USE_STRING_ORDER
	const UINT order = (pWList->iStartLen > NP2_AUTOC_ORDER_LENGTH) ? 0 : pWList->WL_OrderFunc(pWord, len);
#endif
	if (root == NULL) {
		struct WordNode *node;
		node = pWList->nodeCache + pWList->cacheIndex++;
		node->word = pWList->buffer + pWList->offset;

		CopyMemory(node->word, pWord, len);
#if NP2_AUTOC_USE_STRING_ORDER
		node->order = order;
#endif
		node->len = len;
		node->level = 1;
		root = node;
	} else {
		struct WordNode *iter = root;
		struct WordNode *path[NP2_TREE_HEIGHT_LIMIT] = { NULL };
		int top = 0;
		int dir;

		// find a spot and save the path
		for (;;) {
			path[top++] = iter;
#if NP2_AUTOC_USE_STRING_ORDER
			dir = (int)(iter->order - order);
			if (dir == 0 && (len > NP2_AUTOC_ORDER_LENGTH || iter->len > NP2_AUTOC_ORDER_LENGTH)) {
				dir = pWList->WL_strcmp(iter->word, pWord);
			}
#else
			dir = pWList->WL_strcmp(iter->word, pWord);
#endif
			if (dir == 0) {
				return;
			}
			dir = dir < 0;
			if (iter->link[dir] == NULL) {
				break;
			}
			iter = iter->link[dir];
		}

		if (pWList->cacheIndex + 1 > pWList->cacheCapacity) {
			pWList->cacheBytes <<= 1;
			WordList_AddCache(pWList);
		}
		if (pWList->capacity < pWList->offset + len + 1) {
			pWList->capacity <<= 1;
			WordList_AddBuffer(pWList);
		}

		struct WordNode *node = pWList->nodeCache + pWList->cacheIndex++;
		node->word = pWList->buffer + pWList->offset;

		CopyMemory(node->word, pWord, len);
#if NP2_AUTOC_USE_STRING_ORDER
		node->order = order;
#endif
		node->len = len;
		node->level = 1;
		iter->link[dir] = node;

		// walk back and rebalance
		while (--top >= 0) {
			// which child?
			if (top != 0) {
				dir = path[top - 1]->right == path[top];
			}
			aa_tree_skew(path[top]);
			aa_tree_split(path[top]);
			// fix the parent
			if (top != 0) {
				path[top - 1]->link[dir] = path[top];
			} else {
				root = path[top];
			}
		}
	}

	pWList->pListHead = root;
	pWList->nWordCount++;
	pWList->nTotalLen += len + 1;
	pWList->offset += align_up(len + 1);
}

void WordList_Free(struct WordList *pWList) {
	for (int i = 0; i < pWList->cacheCount; i++) {
		NP2HeapFree(pWList->nodeCacheList[i]);
	}
	for (int i = 0; i < pWList->bufferCount; i++) {
		NP2HeapFree(pWList->bufferList[i]);
	}
}

char* WordList_GetList(struct WordList *pWList) {
	struct WordNode *root = pWList->pListHead;
	struct WordNode *path[NP2_TREE_HEIGHT_LIMIT] = { NULL };
	int top = 0;
	char *buf = (char *)NP2HeapAlloc(pWList->nTotalLen + 1);// additional separator
	char * const pList = buf;

	while (root || top > 0) {
		if (root) {
			path[top++] = root;
			root = root->left;
		} else {
			root = path[--top];
			CopyMemory(buf, root->word, root->len);
			buf += root->len;
			*buf++ = '\n'; // the separator char
			root = root->right;
		}
	}
	// trim last separator char
	if (buf != pList) {
		*(--buf) = '\0';
	}
	return pList;
}

struct WordList *WordList_Alloc(LPCSTR pRoot, int iRootLen, BOOL bIgnoreCase) {
	struct WordList *pWList = (struct WordList *)NP2HeapAlloc(sizeof(struct WordList));
	pWList->pListHead = NULL;
	pWList->pWordStart = pRoot;
	pWList->nWordCount = 0;
	pWList->nTotalLen = 0;
	pWList->iStartLen = iRootLen;

	if (bIgnoreCase) {
		pWList->WL_strcmp = _stricmp;
		pWList->WL_strncmp = _strnicmp;
#if NP2_AUTOC_USE_STRING_ORDER
		pWList->WL_OrderFunc = WordList_OrderCase;
#endif
	} else {
		pWList->WL_strcmp = strcmp;
		pWList->WL_strncmp = strncmp;
#if NP2_AUTOC_USE_STRING_ORDER
		pWList->WL_OrderFunc = WordList_Order;
#endif
	}
#if NP2_AUTOC_USE_STRING_ORDER
	pWList->orderStart = pWList->WL_OrderFunc(pRoot, iRootLen);
#endif

	pWList->capacity = NP2_AUTOC_INIT_BUF_SIZE;
	WordList_AddBuffer(pWList);
	pWList->cacheBytes = NP2_AUTOC_INIT_CACHE_BYTES;
	WordList_AddCache(pWList);
	return pWList;
}

static inline void WordList_UpdateRoot(struct WordList *pWList, LPCSTR pRoot, int iRootLen) {
	pWList->pWordStart = pRoot;
	pWList->iStartLen = iRootLen;
#if NP2_AUTOC_USE_STRING_ORDER
	pWList->orderStart = pWList->WL_OrderFunc(pRoot, iRootLen);
#endif
}

static inline BOOL WordList_StartsWith(const struct WordList *pWList, LPCSTR pWord) {
#if NP2_AUTOC_USE_STRING_ORDER
	if (pWList->iStartLen > NP2_AUTOC_ORDER_LENGTH) {
		return pWList->WL_strncmp(pWList->pWordStart, pWord, pWList->iStartLen) == 0;
	}
	if (pWList->orderStart != pWList->WL_OrderFunc(pWord, pWList->iStartLen)) {
		return FALSE;
	}
	return TRUE;
#else
	return pWList->WL_strncmp(pWList->pWordStart, pWord, pWList->iStartLen) == 0;
#endif
}

void WordList_AddListEx(struct WordList *pWList, LPCSTR pList) {
	//StopWatch watch;
	//StopWatch_Start(watch);
	char *word = pWList->wordBuf;
	const int iStartLen = pWList->iStartLen;
	int len = 0;
	BOOL ok = FALSE;
	do {
		const char *sub = strpbrk(pList, " \t.,();^\n\r");
		if (sub) {
			int lenSub = (int)(sub - pList);
			lenSub = min_i(NP2_AUTOC_MAX_WORD_LENGTH - len, lenSub);
			memcpy(word + len, pList, lenSub);
			len += lenSub;
			if (len >= iStartLen) {
				if (*sub == '(') {
					word[len++] = '(';
					word[len++] = ')';
				}
				word[len] = 0;
				if (ok || WordList_StartsWith(pWList, word)) {
					WordList_AddWord(pWList, word, len);
					ok = *sub == '.';
				}
			}
			if (*sub == '^') {
				word[len++] = ' ';
			} else if (!ok && *sub != '.') {
				len = 0;
			} else {
				word[len++] = '.';
			}
			pList = ++sub;
		} else {
			int lenSub = (int)strlen(pList);
			lenSub = min_i(NP2_AUTOC_MAX_WORD_LENGTH - len, lenSub);
			if (len) {
				memcpy(word + len, pList, lenSub);
				len += lenSub;
				word[len] = '\0';
				pList = word;
			} else {
				len = lenSub;
			}
			if (len >= iStartLen) {
				if (ok || WordList_StartsWith(pWList, pList)) {
					WordList_AddWord(pWList, pList, len);
				}
			}
			break;
		}
	} while (*pList);

	//StopWatch_Stop(watch);
	//const double duration = StopWatch_Get(&watch);
	//printf("%s duration=%.6f\n", __func__, duration);
}

static inline void WordList_AddList(struct WordList *pWList, LPCSTR pList) {
	if (StrNotEmptyA(pList)) {
		WordList_AddListEx(pWList, pList);
	}
}

void WordList_AddSubWord(struct WordList *pWList, LPSTR pWord, int wordLength, int iRootLen) {
	/*
	when pRoot is 'b', split 'bugprone-branch-clone' as following:
	1. first hyphen: 'bugprone-branch-clone' => 'bugprone', 'branch-clone'.
	2. second hyphen: 'bugprone-branch-clone' => 'bugprone-branch'; 'branch-clone' => 'branch'.
	*/

	LPCSTR words[8];
	int starts[8];
	UINT count = 0;

	for (int i = 0; i < wordLength - 1; i++) {
		const char ch = pWord[i];
		if (ch == '.' || ch == '-' || ch == ':') {
			if (i >= iRootLen) {
				pWord[i] = '\0';
				WordList_AddWord(pWList, pWord, i);
				for (UINT j = 0; j < count; j++) {
					const int subLen = i - starts[j];
					if (subLen >= iRootLen) {
						WordList_AddWord(pWList, words[j], subLen);
					}
				}
				pWord[i] = ch;
			}
			if (ch != '.' && (pWord[i + 1] == '>' || pWord[i + 1] == ':')) {
				++i;
			}

			const int subLen = wordLength - (i + 1);
			LPCSTR pSubRoot = pWord + i + 1;
			if (subLen >= iRootLen && WordList_StartsWith(pWList, pSubRoot)) {
				WordList_AddWord(pWList, pSubRoot, subLen);
				if (count < COUNTOF(words)) {
					words[count] = pSubRoot;
					starts[count] = i + 1;
					++count;
				}
			}
		}
	}
}


static inline BOOL IsEscapeChar(int ch) {
	return ch == 't' || ch == 'n' || ch == 'r' || ch == 'a' || ch == 'b' || ch == 'v' || ch == 'f'
		|| ch == '0'
		|| ch == '$'; // PHP
	// x u U
}

static inline BOOL IsCppCommentStyle(int style) {
	return style == SCE_C_COMMENT
		|| style == SCE_C_COMMENTLINE
		|| style == SCE_C_COMMENTDOC
		|| style == SCE_C_COMMENTLINEDOC
		|| style == SCE_C_COMMENTDOC_TAG
		|| style == SCE_C_COMMENTDOC_TAG_XML;
}

static inline BOOL IsSpecialStart(int ch) {
	return ch == ':' || ch == '.' || ch == '#' || ch == '@'
		|| ch == '<' || ch == '\\' || ch == '/' || ch == '-'
		|| ch == '>' || ch == '$' || ch == '%';
}

static inline BOOL IsSpecialStartChar(int ch, int chPrev) {
	return (ch == '.')	// member
		|| (ch == '#')	// preprocessor
		|| (ch == '@') // Java/PHP/Doxygen Doc Tag
		// ObjC Keyword, Java Annotation, Python Decorator, Cobra Directive
		|| (ch == '<') // HTML/XML Tag, C# Doc Tag
		|| (ch == '\\')// Doxygen Doc Tag, LaTeX Command
		|| (chPrev == '\\' && (ch == '^' || ch == ':'))// LaTeX input, Emoji input
		// TODO: show emoji list after typing ':'.
		|| (chPrev == '<' && ch == '/')	// HTML/XML Close Tag
		|| (chPrev == '-' && ch == '>')	// member(C/C++/PHP)
		|| (chPrev == ':' && ch == ':');// namespace(C++), static member(C++/Java8/PHP)
}

//=============================================================================
//
// EditCompleteWord()
// Auto-complete words
//
extern EditAutoCompletionConfig autoCompletionConfig;

// CharClassify::SetDefaultCharClasses()
static inline BOOL IsDefaultWordChar(int ch) {
	return ch >= 0x80 || IsAlphaNumeric(ch) || ch == '_';
}

BOOL IsDocWordChar(int ch) {
	if (IsAlphaNumeric(ch) || ch == '_' || ch == '.') {
		return TRUE;
	}

	switch (pLexCurrent->rid) {
	case NP2LEX_TEXTFILE:
	case NP2LEX_2NDTEXTFILE:
	case NP2LEX_ANSI:
	case NP2LEX_BLOCKDIAG:
	case NP2LEX_GRAPHVIZ:
	case NP2LEX_LISP:
	case NP2LEX_SMALI:
		return (ch == '-');

	case NP2LEX_ASM:
	case NP2LEX_FORTRAN:
		return (ch == '#' || ch == '%');
	case NP2LEX_AU3:
		return (ch == '#' || ch == '$' || ch == '@');

	case NP2LEX_BASH:
	case NP2LEX_BATCH:
	case NP2LEX_HTML:
	case NP2LEX_GN:
	case NP2LEX_PHP:
	case NP2LEX_PS1:
		return (ch == '-' || ch == '$');

	case NP2LEX_CIL:
	case NP2LEX_VERILOG:
		return (ch == '$');

	case NP2LEX_CPP:
		return (ch == '#' || ch == '@' || ch == ':');

	case NP2LEX_CSHARP:
	case NP2LEX_HAXE:
	case NP2LEX_SWIFT:
		return (ch == '#' || ch == '@');

	case NP2LEX_CSS:
		return (ch == '-' || ch == '$' || ch == '@');

	case NP2LEX_D:
	case NP2LEX_FSHARP:
	case NP2LEX_INNO:
	case NP2LEX_VB:
		return (ch == '#');

	case NP2LEX_AWK:
	case NP2LEX_JAVA:
	case NP2LEX_JULIA:
	case NP2LEX_KOTLIN:
	case NP2LEX_RUST:
		return (ch == '$' || ch == '@' || ch == ':');

	case NP2LEX_JAVASCRIPT:
	case NP2LEX_TYPESCRIPT:
		return ch == '$' || ch == '#' || ch == '@';

	case NP2LEX_DART:
	case NP2LEX_GRADLE:
	case NP2LEX_GROOVY:
	case NP2LEX_SCALA:
	case NP2LEX_PYTHON:
	case NP2LEX_PERL:
	case NP2LEX_RUBY:
	case NP2LEX_SQL:
	case NP2LEX_TCL:
		return (ch == '$' || ch == '@');

	case NP2LEX_LLVM:
	case NP2LEX_WASM:
		return (ch == '@' || ch == '%' || ch == '$' || ch == '-');

	case NP2LEX_MAKE:
	case NP2LEX_NSIS:
		return (ch == '-' || ch == '$' || ch == '!');

	case NP2LEX_REBOL:
		// http://www.rebol.com/r3/docs/guide/code-syntax.html#section-4
		return (ch == '-' || ch == '!' || ch == '?' || ch == '~' || ch == '+' || ch == '&' || ch == '*' || ch == '=');

	case NP2LEX_XML:
		return (ch == '-' || ch == ':');
	}
	return FALSE;
}

BOOL IsAutoCompletionWordCharacter(int ch) {
	if (ch < 0x80) {
		return IsDocWordChar(ch);
	}
	const CharacterClass cc = SciCall_GetCharacterClass(ch);
	return cc == CharacterClass_Word;
}

static inline BOOL IsWordStyleToIgnore(int style) {
	switch (pLexCurrent->iLexer) {
	case SCLEX_CPP:
		return style == SCE_C_WORD
			|| style == SCE_C_WORD2
			|| style == SCE_C_PREPROCESSOR;

	case SCLEX_GO:
		return style == SCE_GO_WORD
			|| style == SCE_GO_WORD2
			|| style == SCE_GO_BUILTIN_FUNC
			|| style == SCE_GO_FORMAT_SPECIFIER;

	case SCLEX_JSON:
		return style == SCE_JSON_KEYWORD;

	case SCLEX_PYTHON:
		return style == SCE_PY_WORD
			|| style == SCE_PY_WORD2
			|| style == SCE_PY_BUILTIN_CONSTANT
			|| style == SCE_PY_BUILTIN_FUNCTION
			|| style == SCE_PY_ATTRIBUTE
			|| style == SCE_PY_OBJECT_FUNCTION;

	case SCLEX_SMALI:
		return style == SCE_SMALI_WORD
			|| style == SCE_SMALI_DIRECTIVE
			|| style == SCE_SMALI_INSTRUCTION;
	case SCLEX_SQL:
		return style == SCE_SQL_WORD
			|| style == SCE_SQL_WORD2
			|| style == SCE_SQL_USER1
			|| style == SCE_SQL_HEX			// BLOB Hex
			|| style == SCE_SQL_HEX2;		// BLOB Hex
	}
	return FALSE;
}

// https://en.wikipedia.org/wiki/Printf_format_string
static inline BOOL IsStringFormatChar(int ch, int style) {
	if (!IsAlpha(ch)) {
		return FALSE;
	}
	switch (pLexCurrent->iLexer) {
	case SCLEX_CPP:
		return style != SCE_C_OPERATOR;

	case SCLEX_FSHARP:
		return style != SCE_FSHARP_OPERATOR;

	case SCLEX_JULIA:
		return style != SCE_JULIA_OPERATOR && style != SCE_JULIA_OPERATOR2;

	case SCLEX_LUA:
		return style != SCE_LUA_OPERATOR;

	case SCLEX_MATLAB:
		return style != SCE_MAT_OPERATOR;

	case SCLEX_PERL:
		return style != SCE_PL_OPERATOR;
	case SCLEX_PYTHON:
		return style != SCE_PY_OPERATOR;

	case SCLEX_RUBY:
		return style != SCE_RB_OPERATOR;

	case SCLEX_TCL:
		return style != SCE_TCL_OPERATOR;
	}
	return FALSE;
}

static inline BOOL IsEscapeCharEx(int ch, int style) {
	if (!IsEscapeChar(ch)) {
		return FALSE;
	}
	switch (pLexCurrent->iLexer) {
	case SCLEX_NULL:
	case SCLEX_BATCH:
	case SCLEX_CONF:
	case SCLEX_DIFF:
	case SCLEX_MAKEFILE:
	case SCLEX_PROPERTIES:
		return FALSE;

	case SCLEX_CPP:
		return !(style == SCE_C_STRINGRAW || style == SCE_C_VERBATIM || style == SCE_C_COMMENTDOC_TAG);

	case SCLEX_CSHARP:
		return !(style == SCE_CSHARP_VERBATIM_STRING || style == SCE_CSHARP_INTERPOLATED_VERBATIM_STRING);

	case SCLEX_PYTHON:
		// not in raw string
		return !(style >= SCE_PY_STRING_SQ && (style & 7) > 3);
	}
	return TRUE;
}

static inline BOOL NeedSpaceAfterKeyword(const char *word, Sci_Position length) {
	const char *p = strstr(
		" if for try using while elseif switch foreach synchronized "
		, word);
	return p != NULL && p[-1] == ' ' && p[length] == ' ';
}

#define HTML_TEXT_BLOCK_TAG		0
#define HTML_TEXT_BLOCK_CDATA	1
#define HTML_TEXT_BLOCK_JS		2
#define HTML_TEXT_BLOCK_VBS		3
#define HTML_TEXT_BLOCK_PYTHON	4
#define HTML_TEXT_BLOCK_PHP		5
#define HTML_TEXT_BLOCK_CSS		6
#define HTML_TEXT_BLOCK_SGML	7

enum {
	InnoLineStatePreprocessor = 8,
	InnoLineStateCodeSection = 16,
};

extern EDITLEXER lexCSS;
extern EDITLEXER lexHTML;
extern EDITLEXER lexJavaScript;
extern EDITLEXER lexJulia;
extern EDITLEXER lexPHP;
extern EDITLEXER lexPython;
extern EDITLEXER lexVBS;
extern HANDLE idleTaskTimer;

static int GetCurrentHtmlTextBlockEx(int iCurrentStyle) {
	if (iCurrentStyle == SCE_H_CDATA) {
		return HTML_TEXT_BLOCK_CDATA;
	}
	if ((iCurrentStyle >= SCE_HJ_START && iCurrentStyle <= SCE_HJ_TEMPLATELITERAL)
		|| (iCurrentStyle >= SCE_HJA_START && iCurrentStyle <= SCE_HJA_TEMPLATELITERAL)) {
		return HTML_TEXT_BLOCK_JS;
	}
	if ((iCurrentStyle >= SCE_HB_START && iCurrentStyle <= SCE_HB_OPERATOR)
		|| (iCurrentStyle >= SCE_HBA_START && iCurrentStyle <= SCE_HBA_OPERATOR)) {
		return HTML_TEXT_BLOCK_VBS;
	}
	if ((iCurrentStyle >= SCE_HP_START && iCurrentStyle <= SCE_HP_IDENTIFIER)
		|| (iCurrentStyle >= SCE_HPA_START && iCurrentStyle <= SCE_HPA_IDENTIFIER)) {
		return HTML_TEXT_BLOCK_PYTHON;
	}
	if ((iCurrentStyle >= SCE_HPHP_DEFAULT && iCurrentStyle <= SCE_HPHP_COMPLEX_VARIABLE)) {
		return HTML_TEXT_BLOCK_PHP;
	}
	if ((iCurrentStyle >= SCE_H_SGML_DEFAULT && iCurrentStyle <= SCE_H_SGML_BLOCK_DEFAULT)) {
		return HTML_TEXT_BLOCK_SGML;
	}
	return HTML_TEXT_BLOCK_TAG;
}

static int GetCurrentHtmlTextBlock(void) {
	const Sci_Position iCurrentPos = SciCall_GetCurrentPos();
	const int iCurrentStyle = SciCall_GetStyleAt(iCurrentPos);
	return GetCurrentHtmlTextBlockEx(iCurrentStyle);
}

void EscapeRegex(LPSTR pszOut, LPCSTR pszIn) {
	char ch;
	while ((ch = *pszIn++) != '\0') {
		if (ch == '.'		// any character
			|| ch == '^'	// start of line
			|| ch == '$'	// end of line
			|| ch == '?'	// 0 or 1 times
			|| ch == '*'	// 0 or more times
			|| ch == '+'	// 1 or more times
			|| ch == '[' || ch == ']'
			|| ch == '(' || ch == ')'
			) {
			*pszOut++ = '\\';
		}
		*pszOut++ = ch;
	}
	*pszOut++ = '\0';
}

void AutoC_AddDocWord(struct WordList *pWList, BOOL bIgnoreCase, char prefix) {
	LPCSTR const pRoot = pWList->pWordStart;
	const int iRootLen = pWList->iStartLen;

	// optimization for small string
	char onStack[256];
	char *pFind;
	if (iRootLen * 2 + 32 < (int)sizeof(onStack)) {
		ZeroMemory(onStack, sizeof(onStack));
		pFind = onStack;
	} else {
		pFind = (char *)NP2HeapAlloc(iRootLen * 2 + 32);
	}

	if (prefix) {
		char buf[2] = { prefix, '\0' };
		EscapeRegex(pFind, buf);
	}
	if (iRootLen == 0) {
		// find an identifier
		strcat(pFind, "[A-Za-z0-9_]");
		strcat(pFind, "\\i?");
	} else {
		if (IsDefaultWordChar((uint8_t)pRoot[0])) {
			strcat(pFind, "\\h");
		}
		EscapeRegex(pFind + strlen(pFind), pRoot);
		if (IsDefaultWordChar((uint8_t)pRoot[iRootLen - 1])) {
			strcat(pFind, "\\i?");
		} else {
			strcat(pFind, "\\i");
		}
	}

	const Sci_Position iCurrentPos = SciCall_GetCurrentPos() - iRootLen - (prefix ? 1 : 0);
	const Sci_Position iDocLen = SciCall_GetLength();
	const int findFlag = SCFIND_REGEXP | SCFIND_POSIX | (bIgnoreCase ? 0 : SCFIND_MATCHCASE);
	struct Sci_TextToFind ft = { { 0, iDocLen }, pFind, { 0, 0 } };

	Sci_Position iPosFind = SciCall_FindText(findFlag, &ft);
	HANDLE timer = idleTaskTimer;
	WaitableTimer_Set(timer, autoCompletionConfig.dwScanWordsTimeout);

	while (iPosFind >= 0 && iPosFind < iDocLen && WaitableTimer_Continue(timer)) {
		Sci_Position wordEnd = iPosFind + iRootLen;
		const int style = SciCall_GetStyleAt(wordEnd - 1);
		wordEnd = ft.chrgText.cpMax;
		if (iPosFind != iCurrentPos && !IsWordStyleToIgnore(style)) {
			// find all word after '::', '->', '.' and '-'
			BOOL bSubWord = FALSE;
			while (wordEnd < iDocLen) {
				const int ch = SciCall_GetCharAt(wordEnd);
				if (!(ch == ':' || ch == '.' || ch == '-')) {
					if (ch == '!' && pLexCurrent->iLexer == SCLEX_RUST && style == SCE_RUST_MACRO) {
						// macro: println!()
						++wordEnd;
					}
					break;
				}

				const Sci_Position before = wordEnd;
				Sci_Position width = 0;
				int chNext = SciCall_GetCharacterAndWidth(wordEnd + 1, &width);
				if ((ch == '-' && chNext == '>') || (ch == ':' && chNext == ':')) {
					chNext = SciCall_GetCharacterAndWidth(wordEnd + 2, &width);
					if (IsAutoCompletionWordCharacter(chNext)) {
						wordEnd += 2;
					}
				} else if (ch == '.' || (ch == '-' && style == SciCall_GetStyleAt(wordEnd))) {
					if (IsAutoCompletionWordCharacter(chNext)) {
						++wordEnd;
					}
				}
				if (wordEnd == before) {
					break;
				}

				while (wordEnd < iDocLen && !IsDefaultWordChar(chNext)) {
					wordEnd += width;
					chNext = SciCall_GetCharacterAndWidth(wordEnd, &width);
					if (!IsAutoCompletionWordCharacter(chNext)) {
						break;
					}
				}

				wordEnd = SciCall_WordEndPosition(wordEnd, TRUE);
				if (wordEnd - iPosFind > NP2_AUTOC_MAX_WORD_LENGTH) {
					wordEnd = before;
					break;
				}
				bSubWord = TRUE;
			}

			if (wordEnd - iPosFind >= iRootLen) {
				char *pWord = pWList->wordBuf + NP2DefaultPointerAlignment;
				BOOL bChanged = FALSE;
				struct Sci_TextRange tr = { { iPosFind, min_pos(iPosFind + NP2_AUTOC_MAX_WORD_LENGTH, wordEnd) }, pWord };
				int wordLength = (int)SciCall_GetTextRange(&tr);

				Sci_Position before = SciCall_PositionBefore(iPosFind);
				if (before + 1 == iPosFind) {
					const int ch = SciCall_GetCharAt(before);
					if (ch == '\\') { // word after escape char
						before = SciCall_PositionBefore(before);
						const int chPrev = (before + 2 == iPosFind) ? SciCall_GetCharAt(before) : 0;
						if (chPrev != '\\' && IsEscapeCharEx(*pWord, SciCall_GetStyleAt(before))) {
							pWord++;
							--wordLength;
							bChanged = TRUE;
						}
					} else if (ch == '%') { // word after format char
						if (IsStringFormatChar(*pWord, SciCall_GetStyleAt(before))) {
							pWord++;
							--wordLength;
							bChanged = TRUE;
						}
					}
				}
				if (prefix && prefix == *pWord) {
					pWord++;
					--wordLength;
					bChanged = TRUE;
				}

				//if (pLexCurrent->rid == NP2LEX_PHP && wordLength >= 2 && *pWord == '$' && pWord[1] == '$') {
				//	pWord++;
				//	--wordLength;
				//	bChanged = TRUE;
				//}
				while (wordLength > 0 && (pWord[wordLength - 1] == '-' || pWord[wordLength - 1] == ':' || pWord[wordLength - 1] == '.')) {
					--wordLength;
					pWord[wordLength] = '\0';
				}
				if (bChanged) {
					CopyMemory(pWList->wordBuf, pWord, wordLength + 1);
					pWord = pWList->wordBuf;
				}

				bChanged = wordLength >= iRootLen && WordList_StartsWith(pWList, pWord);
				if (bChanged && !(pWord[0] == ':' && pWord[1] != ':')) {
					BOOL space = FALSE;
					if (!(pLexCurrent->iLexer == SCLEX_CPP && style == SCE_C_MACRO)) {
						while (IsASpaceOrTab(SciCall_GetCharAt(wordEnd))) {
							space = TRUE;
							wordEnd++;
						}
					}

					const int chWordEnd = SciCall_GetCharAt(wordEnd);
					if ((pLexCurrent->iLexer == SCLEX_JULIA || pLexCurrent->iLexer == SCLEX_RUST) && chWordEnd == '!') {
						const int chNext = SciCall_GetCharAt(wordEnd + 1);
						if (chNext == '(') {
							wordEnd += 2;
							pWord[wordLength++] = '!';
							pWord[wordLength++] = '(';
							pWord[wordLength++] = ')';
						}
					}
					else if (chWordEnd == '(') {
						if (space && NeedSpaceAfterKeyword(pWord, wordLength)) {
							pWord[wordLength++] = ' ';
						}

						pWord[wordLength++] = '(';
						pWord[wordLength++] = ')';
						wordEnd++;
					}

					if (wordLength >= iRootLen) {
						pWord[wordLength] = '\0';
						WordList_AddWord(pWList, pWord, wordLength);
						if (bSubWord) {
							WordList_AddSubWord(pWList, pWord, wordLength, iRootLen);
						}
					}
				}
			}
		}

		ft.chrg.cpMin = wordEnd;
		iPosFind = SciCall_FindText(findFlag, &ft);
	}

	if (pFind != onStack) {
		NP2HeapFree(pFind);
	}
}

void AutoC_AddKeyword(struct WordList *pWList, int iCurrentStyle) {
	for (int i = 0; i < NUMKEYWORD; i++) {
		const char *pKeywords = pLexCurrent->pKeyWords->pszKeyWords[i];
		if (StrNotEmptyA(pKeywords) && !(currentLexKeywordAttr[i] & KeywordAttr_NoAutoComp)) {
			WordList_AddListEx(pWList, pKeywords);
		}
	}

	// additional keywords
	if (np2_LexKeyword && !(pLexCurrent->iLexer == SCLEX_CPP && !IsCppCommentStyle(iCurrentStyle))) {
		WordList_AddList(pWList, (*np2_LexKeyword)[0]);
		WordList_AddList(pWList, (*np2_LexKeyword)[1]);
		WordList_AddList(pWList, (*np2_LexKeyword)[2]);
		WordList_AddList(pWList, (*np2_LexKeyword)[3]);
	}

	// embedded script
	PEDITLEXER pLex = NULL;
	if (pLexCurrent->rid == NP2LEX_HTML) {
		const int block = GetCurrentHtmlTextBlockEx(iCurrentStyle);
		switch (block) {
		case HTML_TEXT_BLOCK_JS:
			pLex = &lexJavaScript;
			break;
		case HTML_TEXT_BLOCK_VBS:
			pLex = &lexVBS;
			break;
		case HTML_TEXT_BLOCK_PYTHON:
			pLex = &lexPython;
			break;
		case HTML_TEXT_BLOCK_PHP:
			pLex = &lexPHP;
			break;
		case HTML_TEXT_BLOCK_CSS:
			pLex = &lexCSS;
			break;
		}
	} else if (pLexCurrent->rid == NP2LEX_TYPESCRIPT) {
		pLex = &lexJavaScript;
	}
	if (pLex != NULL) {
		for (int i = 0; i < NUMKEYWORD; i++) {
			const char *pKeywords = pLex->pKeyWords->pszKeyWords[i];
			if (StrNotEmptyA(pKeywords)) {
				WordList_AddListEx(pWList, pKeywords);
			}
		}
	}
	if (pLexCurrent->rid == NP2LEX_JAVASCRIPT || pLexCurrent->rid == NP2LEX_TYPESCRIPT) {
		// event handler
		WordList_AddListEx(pWList, lexHTML.pKeyWords->pszKeyWords[7]);
	}
}

#define AutoC_AddSpecWord_Finish	1
#define AutoC_AddSpecWord_Keyword	2
INT AutoC_AddSpecWord(struct WordList *pWList, int iCurrentStyle, int ch, int chPrev) {
#if EnableLaTeXLikeEmojiInput
	if ((ch == '\\' || (chPrev == '\\' && (ch == '^' || ch == ':'))) && autoCompletionConfig.bLaTeXInputMethod) {
		if (ch != ':') {
			WordList_AddListEx(pWList, LaTeXInputSequenceString);
		} else {
			WordList_AddListEx(pWList, EmojiInputSequenceString);
		}
	}
#else
	if ((ch == '\\' || (chPrev == '\\' && ch == '^')) && autoCompletionConfig.bLaTeXInputMethod) {
		WordList_AddListEx(pWList, LaTeXInputSequenceString);
	}
#endif

	switch (pLexCurrent->iLexer) {
	case SCLEX_AHK:
		if (ch == '#' && iCurrentStyle == SCE_AHK_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[1]); // #directive
			return AutoC_AddSpecWord_Finish;
		}
		if (ch == '@' && (iCurrentStyle == SCE_AHK_COMMENTLINE || iCurrentStyle == SCE_AHK_COMMENTBLOCK)) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[2]); // @directive
			return AutoC_AddSpecWord_Finish;
		}
		break;

	case SCLEX_APDL:
		if (iCurrentStyle == 0 && (ch == '*' || ch == '/')) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[(ch == '/') ? 2 : 3]);// slash, star command
			return AutoC_AddSpecWord_Keyword;
		}
		break;

#if 0
	case SCLEX_CSS:
		if (ch == '@' && iCurrentStyle == SCE_CSS_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[1]); // @rule
			return AutoC_AddSpecWord_Keyword;
		}
		if (ch == ':' && iCurrentStyle == SCE_CSS_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[(chPrev == ':') ? 3 : 2]);
			return AutoC_AddSpecWord_Keyword;
		}
		break;
#endif

	case SCLEX_CPP:
		if (IsCppCommentStyle(iCurrentStyle) && np2_LexKeyword) {
			if ((ch == '@' && (np2_LexKeyword == &kwJavaDoc || np2_LexKeyword == &kwPHPDoc || np2_LexKeyword == &kwDoxyDoc))
					|| (ch == '\\' && np2_LexKeyword == &kwDoxyDoc)
					|| ((ch == '<' || chPrev == '<') && np2_LexKeyword == &kwNETDoc)) {
				WordList_AddList(pWList, (*np2_LexKeyword)[0]);
				WordList_AddList(pWList, (*np2_LexKeyword)[1]);
				WordList_AddList(pWList, (*np2_LexKeyword)[2]);
				WordList_AddList(pWList, (*np2_LexKeyword)[3]);
				return AutoC_AddSpecWord_Finish;
			}
		} else if (iCurrentStyle == SCE_C_DEFAULT) {
			if (ch == '#') { // #preprocessor
				const char *pKeywords = pLexCurrent->pKeyWords->pszKeyWords[2];
				if (StrNotEmptyA(pKeywords)) {
					WordList_AddListEx(pWList, pKeywords);
					return AutoC_AddSpecWord_Finish;
				}
			} else if (ch == '@') { // @directive, @annotation, @decorator
				const char *pKeywords = pLexCurrent->pKeyWords->pszKeyWords[3];
				if (StrNotEmptyA(pKeywords)) {
					WordList_AddListEx(pWList, pKeywords);
					// user defined annotation
					return AutoC_AddSpecWord_Keyword;
				}
			}
			//else if (chPrev == ':' && ch == ':') {
			//	WordList_AddList(pWList, "C++/namespace C++/Java8/PHP/static SendMessage()");
			//}
			//else if (chPrev == '-' && ch == '>') {
			//	WordList_AddList(pWList, "C/C++pointer PHP-variable");
			//}
		}
		break;

	case SCLEX_CSHARP:
		if (ch == '#' && iCurrentStyle == SCE_CSHARP_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[3]); // preprocessor
			return AutoC_AddSpecWord_Finish;
		}
		if ((ch == '<' || (chPrev == '<' && ch == '/')) && (iCurrentStyle > SCE_CSHARP_DEFAULT && iCurrentStyle < SCE_CSHARP_TASKMARKER)) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[10]); // comment tag
			return AutoC_AddSpecWord_Finish;
		}
		break;

	case SCLEX_CONF:
	case SCLEX_HTML:
	case SCLEX_XML:
		if (ch == '<' || (chPrev == '<' && ch == '/')) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[0]);// Tag
			if (pLexCurrent->rid == NP2LEX_XML) {
				if (np2_LexKeyword) { // XML Tag
					WordList_AddList(pWList, (*np2_LexKeyword)[0]);
				}
			}
			return AutoC_AddSpecWord_Keyword; // application defined tags
		}
		break;

	case SCLEX_MARKDOWN:
		if (ch == '<' || (chPrev == '<' && ch == '/')) {
			WordList_AddList(pWList, lexHTML.pKeyWords->pszKeyWords[0]);// Tag
			return AutoC_AddSpecWord_Keyword; // custom tags
		}
		break;

	case SCLEX_D:
		if ((ch == '#' || ch == '@') && iCurrentStyle == SCE_D_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[(ch == '#') ? 2 : 3]); // preprocessor, attribute
			return AutoC_AddSpecWord_Finish;
		}
		break;

	case SCLEX_DART:
		if (ch == '@' && iCurrentStyle == SCE_DART_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[4]); // metadata
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_HAXE:
		if (ch == '#' && iCurrentStyle == SCE_HAXE_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[1]); // preprocessor
			return AutoC_AddSpecWord_Finish;
		}
		if (ch == '@' && iCurrentStyle == SCE_HAXE_COMMENTBLOCKDOC) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[8]); // comment
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_INNOSETUP:
		if (ch == '#' && (iCurrentStyle == SCE_INNO_DEFAULT || iCurrentStyle == SCE_INNO_INLINE_EXPANSION)) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[4]); // preprocessor
			return (iCurrentStyle == SCE_INNO_DEFAULT) ? AutoC_AddSpecWord_Finish : AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_GRAPHVIZ:
		if (ch == '<' || (chPrev == '<' && ch == '/')) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[1]);// Tag
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_GROOVY:
	case SCLEX_JAVA:
		if (ch == '@') {
			if (iCurrentStyle == SCE_JAVA_DEFAULT) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[7]); // annotation
				return AutoC_AddSpecWord_Keyword;
			}
			if (iCurrentStyle >= SCE_JAVA_COMMENTBLOCKDOC && iCurrentStyle <= SCE_JAVA_TASKMARKER) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[9]); // Javadoc
				return AutoC_AddSpecWord_Finish;
			}
		}
		break;

	case SCLEX_JAVASCRIPT:
		if (ch == '@' || (ch == '<' && pLexCurrent->rid == NP2LEX_TYPESCRIPT)) {
			if (iCurrentStyle >= SCE_JS_COMMENTLINE && iCurrentStyle <= SCE_JS_TASKMARKER) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[9]); // JSDoc, TSDoc
				return AutoC_AddSpecWord_Finish;
			}
#if 0
			if (ch == '@' && iCurrentStyle == SCE_JS_DEFAULT) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[7]); // decorator
				return AutoC_AddSpecWord_Keyword;
			}
#endif
		}
		break;

	case SCLEX_JULIA:
		if (ch == '@' && iCurrentStyle == SCE_JULIA_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[6]); // macro
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_KOTLIN:
		if (ch == '@') {
			if (iCurrentStyle == SCE_KOTLIN_DEFAULT) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[4]); // annotation
				return AutoC_AddSpecWord_Keyword;
			}
			if (iCurrentStyle >= SCE_KOTLIN_COMMENTLINE && iCurrentStyle <= SCE_KOTLIN_TASKMARKER) {
				WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[6]); // KDoc
				return AutoC_AddSpecWord_Finish;
			}
		}
		break;

	case SCLEX_LATEX:
	case SCLEX_TEXINFO:
		if ((ch == '\\' || (chPrev == '\\' && ch == '^')) && !autoCompletionConfig.bLaTeXInputMethod) {
			WordList_AddListEx(pWList, LaTeXInputSequenceString);
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_PYTHON:
		if (ch == '@' && iCurrentStyle == SCE_PY_DEFAULT) {
			const char *pKeywords = pLexCurrent->pKeyWords->pszKeyWords[7]; // @decorator
			if (StrNotEmptyA(pKeywords)) {
				WordList_AddListEx(pWList, pKeywords);
				return AutoC_AddSpecWord_Keyword;
			}
		}
		break;

	case SCLEX_REBOL:
		if (ch == '#' && iCurrentStyle == SCE_REBOL_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[1]); // directive
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_SMALI:
		if (ch == '.' && iCurrentStyle == SCE_C_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[9]);
			return AutoC_AddSpecWord_Finish;
		}
		break;

	case SCLEX_SWIFT:
		if ((ch == '@' || ch == '#') && iCurrentStyle == SCE_SWIFT_DEFAULT) {
			WordList_AddList(pWList, pLexCurrent->pKeyWords->pszKeyWords[(ch == '#') ? 1 : 2]); // directive, attribute
			return AutoC_AddSpecWord_Keyword;
		}
		break;

	case SCLEX_VB:
		if (ch == '#' && iCurrentStyle == SCE_B_DEFAULT) {
			const char *pKeywords = pLexCurrent->pKeyWords->pszKeyWords[3]; // #preprocessor
			if (StrNotEmptyA(pKeywords)) {
				WordList_AddListEx(pWList, pKeywords);
				return AutoC_AddSpecWord_Finish;
			}
		}
		break;
	}
	return 0;
}

void EditCompleteUpdateConfig(void) {
	int i = 0;
	const int mask = autoCompletionConfig.fAutoCompleteFillUpMask;
	if (mask & AutoCompleteFillUpSpace) {
		autoCompletionConfig.szAutoCompleteFillUp[i++] = ' ';
	}

	const BOOL punctuation = mask & AutoCompleteFillUpPunctuation;
	int k = 0;
	for (UINT j = 0; j < COUNTOF(autoCompletionConfig.wszAutoCompleteFillUp); j++) {
		const WCHAR c = autoCompletionConfig.wszAutoCompleteFillUp[j];
		if (c == L'\0') {
			break;
		}
		if (IsPunctuation(c)) {
			autoCompletionConfig.wszAutoCompleteFillUp[k++] = c;
			if (punctuation) {
				autoCompletionConfig.szAutoCompleteFillUp[i++] = (char)c;
			}
		}
	}

	autoCompletionConfig.szAutoCompleteFillUp[i] = '\0';
	autoCompletionConfig.wszAutoCompleteFillUp[k] = L'\0';
}

static BOOL EditCompleteWordCore(int iCondition, BOOL autoInsert) {
	const Sci_Position iCurrentPos = SciCall_GetCurrentPos();
	const int iCurrentStyle = SciCall_GetStyleAt(iCurrentPos);
	const Sci_Line iLine = SciCall_LineFromPosition(iCurrentPos);
	const Sci_Position iLineStartPos = SciCall_PositionFromLine(iLine);

	// word before current position
	Sci_Position iStartWordPos = iCurrentPos;
	do {
		Sci_Position before = iStartWordPos;
		iStartWordPos = SciCall_WordStartPosition(before, TRUE);
		const BOOL nonWord = iStartWordPos == before;
		before = SciCall_PositionBefore(iStartWordPos);
		if (nonWord) {
			// non-word
			if (before + 1 != iStartWordPos) {
				break;
			}

			const int ch = SciCall_GetCharAt(before);
			if (!IsDocWordChar(ch) || IsSpecialStartChar(ch, '\0')) {
				break;
			}

			iStartWordPos = before;
		} else {
			const Sci_Position iPos = SciCall_WordEndPosition(before, TRUE);
			if (iPos == iStartWordPos) {
				// after CJK word
				break;
			}
		}
	} while (iStartWordPos > iLineStartPos);
	if (iStartWordPos == iCurrentPos) {
		return FALSE;
	}

	// beginning of word
	int ch = SciCall_GetCharAt(iStartWordPos);

	int chPrev = '\0';
	int chPrev2 = '\0';
	if (ch < 0x80 && iStartWordPos > iLineStartPos) {
		Sci_Position before = SciCall_PositionBefore(iStartWordPos);
		if (before + 1 == iStartWordPos) {
			chPrev = SciCall_GetCharAt(before);
			const Sci_Position before2 = SciCall_PositionBefore(before);
			if (before2 >= iLineStartPos && before2 + 1 == before) {
				chPrev2 = SciCall_GetCharAt(before2);
			}
			if ((chPrev == '\\' && chPrev2 != '\\' && IsEscapeCharEx(ch, SciCall_GetStyleAt(before))) // word after escape char
				// word after format char
				|| (chPrev == '%' && IsStringFormatChar(ch, SciCall_GetStyleAt(before)))) {
				++iStartWordPos;
				ch = SciCall_GetCharAt(iStartWordPos);
				chPrev = '\0';
			}
		}
	}

	int iRootLen = autoCompletionConfig.iMinWordLength;
	if (ch >= '0' && ch <= '9') {
		if (autoCompletionConfig.iMinNumberLength <= 0) { // ignore number
			return FALSE;
		}

		iRootLen = autoCompletionConfig.iMinNumberLength;
		if (ch == '0') {
			// number prefix
			const int chNext = SciCall_GetCharAt(iStartWordPos + 1) | 0x20;
			if (chNext == 'x' || chNext == 'b' || chNext == 'o') {
				iRootLen += 2;
			}
		}
	}

	if (iCurrentPos - iStartWordPos < iRootLen) {
		return FALSE;
	}

	// preprocessor like: # space preprocessor
	if (pLexCurrent->rid == NP2LEX_CPP && (chPrev == '#' || IsASpaceOrTab(chPrev))) {
		Sci_Position before = iStartWordPos - 1;
		if (chPrev != '#') {
			while (before >= iLineStartPos) {
				chPrev = SciCall_GetCharAt(before);
				if (!IsASpaceOrTab(chPrev)) {
					break;
				}
				--before;
			}
		}
		if (chPrev == '#') {
			if (before > iLineStartPos) {
				--before;
				while (before >= iLineStartPos && IsASpaceOrTab(SciCall_GetCharAt(before))) {
					--before;
				}
				if (before >= iLineStartPos) {
					chPrev = '\0';
				}
			}
			ch = chPrev;
		}
		chPrev = '\0';
	} else if (IsSpecialStartChar(chPrev, chPrev2)) {
		ch = chPrev;
		chPrev = chPrev2;
	}

	// optimization for small string
	char onStack[128];
	char *pRoot;
	if (iCurrentPos - iStartWordPos + 1 < (Sci_Position)sizeof(onStack)) {
		ZeroMemory(onStack, sizeof(onStack));
		pRoot = onStack;
	} else {
		pRoot = (char *)NP2HeapAlloc(iCurrentPos - iStartWordPos + 1);
	}

	struct Sci_TextRange tr = { { iStartWordPos, iCurrentPos }, pRoot };
	SciCall_GetTextRange(&tr);
	iRootLen = (int)strlen(pRoot);

#if 0
	StopWatch watch;
	StopWatch_Start(watch);
#endif

	BOOL bIgnore = iRootLen != 0 && (pRoot[0] >= '0' && pRoot[0] <= '9'); // number
	const BOOL bIgnoreCase = bIgnore || autoCompletionConfig.bIgnoreCase;
	struct WordList *pWList = WordList_Alloc(pRoot, iRootLen, bIgnoreCase);
	BOOL bIgnoreDoc = FALSE;
	char prefix = '\0';

	if (!bIgnore && IsSpecialStartChar(ch, chPrev)) {
		const int result = AutoC_AddSpecWord(pWList, iCurrentStyle, ch, chPrev);
		if (result == AutoC_AddSpecWord_Finish) {
			bIgnore = TRUE;
			bIgnoreDoc = TRUE;
		} else if (result == AutoC_AddSpecWord_Keyword) {
			bIgnore = TRUE;
			// HTML/XML Tag
			if (ch == '/' || ch == '>') {
				ch = '<';
			}
			prefix = (char)ch;
		}
	}

	BOOL retry = FALSE;
	const BOOL bScanWordsInDocument = autoCompletionConfig.bScanWordsInDocument;
	do {
		if (!bIgnore) {
			// keywords
			AutoC_AddKeyword(pWList, iCurrentStyle);
		}
		if (bScanWordsInDocument) {
			if (!bIgnoreDoc || pWList->nWordCount == 0) {
				AutoC_AddDocWord(pWList, bIgnoreCase, prefix);
			}
			if (prefix && pWList->nWordCount == 0) {
				prefix = '\0';
				AutoC_AddDocWord(pWList, bIgnoreCase, prefix);
			}
		}

		retry = FALSE;
		if (pWList->nWordCount == 0 && iRootLen != 0) {
			const char *pSubRoot = strpbrk(pWList->pWordStart, ":.#@<\\/->$%");
			if (pSubRoot) {
				while (IsSpecialStart(*pSubRoot)) {
					pSubRoot++;
				}
				if (*pSubRoot) {
					iRootLen = (int)strlen(pSubRoot);
					WordList_UpdateRoot(pWList, pSubRoot, iRootLen);
					retry = TRUE;
					bIgnore = FALSE;
					bIgnoreDoc = FALSE;
					prefix = '\0';
				}
			}
		}
	} while (retry);

#if 0
	StopWatch_Stop(watch);
	const double elapsed = StopWatch_Get(&watch);
	printf("Notepad2 AddDocWord(%u, %u): %.6f\n", pWList->nWordCount, pWList->nTotalLen, elapsed);
#endif

	const BOOL bShow = pWList->nWordCount > 0 && !(pWList->nWordCount == 1 && pWList->nTotalLen == (UINT)(iRootLen + 1));
	const BOOL bUpdated = (autoCompletionConfig.iPreviousItemCount == 0)
		// deleted some words. leave some words that no longer matches current input at the top.
		|| (iCondition == AutoCompleteCondition_OnCharAdded && autoCompletionConfig.iPreviousItemCount - pWList->nWordCount > autoCompletionConfig.iVisibleItemCount)
		// added some words. TODO: check top matched items before updating, if top items not changed, delay the update.
		|| (iCondition == AutoCompleteCondition_OnCharDeleted && autoCompletionConfig.iPreviousItemCount < pWList->nWordCount);

	if (bShow && bUpdated) {
		autoCompletionConfig.iPreviousItemCount = pWList->nWordCount;
		char *pList = WordList_GetList(pWList);
		SciCall_AutoCSetOptions(SC_AUTOCOMPLETE_FIXED_SIZE);
		SciCall_AutoCSetOrder(SC_ORDER_PRESORTED); // pre-sorted
		SciCall_AutoCSetIgnoreCase(bIgnoreCase); // case sensitivity
		//if (bIgnoreCase) {
		//	SciCall_AutoCSetCaseInsensitiveBehaviour(SC_CASEINSENSITIVEBEHAVIOUR_IGNORECASE);
		//}
		SciCall_AutoCSetSeparator('\n');
		SciCall_AutoCSetFillUps(autoCompletionConfig.szAutoCompleteFillUp);
		//SciCall_AutoCSetDropRestOfWord(TRUE); // delete orginal text: pRoot
		SciCall_AutoCSetMaxHeight(min_u(pWList->nWordCount, autoCompletionConfig.iVisibleItemCount)); // visible rows
		SciCall_AutoCSetCancelAtStart(FALSE); // don't cancel the list when deleting character
		SciCall_AutoCSetChooseSingle(autoInsert);
		SciCall_AutoCShow(pWList->iStartLen, pList);
		NP2HeapFree(pList);
	}

	if (pRoot != onStack) {
		NP2HeapFree(pRoot);
	}
	WordList_Free(pWList);
	NP2HeapFree(pWList);
	return bShow;
}

void EditCompleteWord(int iCondition, BOOL autoInsert) {
	if (iCondition == AutoCompleteCondition_OnCharAdded) {
		if (autoCompletionConfig.iPreviousItemCount <= 2*autoCompletionConfig.iVisibleItemCount) {
			return;
		}
		// too many words in auto-completion list, recreate it.
	}

	if (iCondition == AutoCompleteCondition_Normal) {
		autoCompletionConfig.iPreviousItemCount = 0; // recreate list
	}

	BOOL bShow = EditCompleteWordCore(iCondition, autoInsert);
	if (!bShow) {
		autoCompletionConfig.iPreviousItemCount = 0;
		if (iCondition != AutoCompleteCondition_Normal) {
			SciCall_AutoCCancel();
		}
	}
}

static BOOL CanAutoCloseSingleQuote(int chPrev, int iCurrentStyle) {
	const int iLexer = pLexCurrent->iLexer;
	if (chPrev >= 0x80	// someone's
		|| (iLexer == SCLEX_CPP && iCurrentStyle == SCE_C_NUMBER)
		|| (iLexer == SCLEX_JULIA && iCurrentStyle == SCE_MAT_OPERATOR) // transpose operator
		|| (iLexer == SCLEX_LISP && iCurrentStyle == SCE_C_OPERATOR)
		|| (iLexer == SCLEX_MATLAB && iCurrentStyle == SCE_MAT_OPERATOR) // transpose operator
		|| (iLexer == SCLEX_PERL && iCurrentStyle == SCE_PL_OPERATOR && chPrev == '&') // SCE_PL_IDENTIFIER
		|| ((iLexer == SCLEX_VB || iLexer == SCLEX_VBSCRIPT) && (iCurrentStyle == SCE_B_DEFAULT || iCurrentStyle == SCE_B_COMMENT))
		|| (iLexer == SCLEX_VERILOG && (iCurrentStyle == SCE_V_NUMBER || iCurrentStyle == SCE_V_DEFAULT))
		|| (iLexer == SCLEX_TEXINFO && iCurrentStyle == SCE_L_DEFAULT && chPrev == '@') // SCE_L_SPECIAL
	) {
		return FALSE;
	}

	// someone's, don't
	if (IsAlphaNumeric(chPrev)) {
		// character prefix
		if (pLexCurrent->rid == NP2LEX_CPP || pLexCurrent->rid == NP2LEX_RC || iLexer == SCLEX_PYTHON || iLexer == SCLEX_SQL || iLexer == SCLEX_RUST) {
			const int lower = chPrev | 0x20;
			const int chPrev2 = SciCall_GetCharAt(SciCall_GetCurrentPos() - 3);
			const BOOL bSubWord = chPrev2 >= 0x80 || IsAlphaNumeric(chPrev2);

			switch (iLexer) {
			case SCLEX_CPP:
				return (lower == 'u' || chPrev == 'L') && !bSubWord;

			case SCLEX_PYTHON: {
				const int lower2 = chPrev2 | 0x20;
				return (lower == 'r' || lower == 'u' || lower == 'b' || lower == 'f') && (!bSubWord || (lower != lower2 && (lower2 == 'r' || lower2 == 'u' || lower2 == 'b' || lower2 == 'f')));
			}

			case SCLEX_SQL:
				return (lower == 'q' || lower == 'x' || lower == 'b') && !bSubWord;

			case SCLEX_RUST:
				return chPrev == 'b' && !bSubWord; // bytes
			}
		}
		return FALSE;
	}
	if (iLexer == SCLEX_RUST || iLexer == SCLEX_REBOL) {
		// Rust lifetime, REBOL symbol
		return FALSE;
	}

	return TRUE;
}

BOOL EditIsOpenBraceMatched(Sci_Position pos, Sci_Position startPos) {
	// SciCall_GetEndStyled() is SciCall_GetCurrentPos() - 1
#if 0
	// style current line, ensure brace matching on current line matched with style
	const Sci_Line iLine = SciCall_LineFromPosition(pos);
	SciCall_EnsureStyledTo(SciCall_PositionFromLine(iLine + 1));
#else
	// only find close brace with same style in next 1KiB text
	const Sci_Position iDocLen = SciCall_GetLength();
	SciCall_EnsureStyledTo(min_pos(iDocLen, pos + 1024));
#endif
	// find next close brace
	const Sci_Position iPos = SciCall_BraceMatchNext(pos, startPos);
	if (iPos >= 0) {
		// style may not matched when iPos > SciCall_GetEndStyled() (e.g. iPos on next line), see Document::BraceMatch()
#if 0
		SciCall_EnsureStyledTo(iPos + 1);
#endif
		// TODO: retry when style not matched
		if (SciCall_GetStyleAt(pos) == SciCall_GetStyleAt(iPos)) {
			// check whether next close brace already matched
			return pos == 0 || SciCall_BraceMatchNext(iPos, SciCall_PositionBefore(pos)) < 0;
		}
	}
	return FALSE;
}

static inline int GetCharacterStyle(int iLexer) {
	switch (iLexer) {
	case SCLEX_CPP:
		return SCE_C_CHARACTER;
	case SCLEX_CSHARP:
		return SCE_CSHARP_CHARACTER;
	case SCLEX_JAVA:
		return SCE_JAVA_CHARACTER;
	case SCLEX_GO:
		return SCE_GO_CHARACTER;
	case SCLEX_KOTLIN:
		return SCE_KOTLIN_CHARACTER;
	case SCLEX_RUST:
		return SCE_RUST_CHARACTER;
	default:
		// single quoted string, not character literal
		return 0;
	}
}

static inline int GetGenericOperatorStyle(int iLexer) {
	switch (iLexer) {
	case SCLEX_CPP:
		return SCE_C_OPERATOR;
	case SCLEX_CSHARP:
		return SCE_CSHARP_OPERATOR;
	case SCLEX_DART:
		return SCE_DART_OPERATOR;
	case SCLEX_GROOVY:
		return SCE_GROOVY_OPERATOR;
	case SCLEX_HAXE:
		return SCE_HAXE_OPERATOR;
	case SCLEX_JAVA:
		return SCE_JAVA_OPERATOR;
	case SCLEX_JAVASCRIPT:
		return SCE_JS_OPERATOR;
	case SCLEX_KOTLIN:
		return SCE_KOTLIN_OPERATOR;
	case SCLEX_RUST:
		return SCE_RUST_CHARACTER;
	case SCLEX_SWIFT:
		return SCE_SWIFT_OPERATOR;
	default:
		// '<>' is not generic or template
		return 0;
	}
}

static inline BOOL IsGenericTypeStyle(int iLexer, int style) {
	switch (iLexer) {
	case SCLEX_CPP:
		return style == SCE_C_CLASS
			|| style == SCE_C_INTERFACE
			|| style == SCE_C_STRUCT
			|| style == SCE_C_WORD2;
	case SCLEX_CSHARP:
		return style == SCE_CSHARP_CLASS
			|| style == SCE_CSHARP_INTERFACE
			|| style == SCE_CSHARP_STRUCT
			|| style == SCE_CSHARP_ENUM
			|| style == SCE_CSHARP_WORD2;
	case SCLEX_DART:
		return style == SCE_DART_CLASS
			|| style == SCE_DART_ENUM
			|| style == SCE_DART_WORD2;
	case SCLEX_GROOVY:
		return style == SCE_GROOVY_CLASS
			|| style == SCE_GROOVY_INTERFACE
			|| style == SCE_GROOVY_TRAIT
			|| style == SCE_GROOVY_ENUM;
	case SCLEX_HAXE:
		return style == SCE_HAXE_CLASS
			|| style == SCE_HAXE_INTERFACE
			|| style == SCE_HAXE_ENUM;
	case SCLEX_JAVA:
		return style == SCE_JAVA_CLASS
			|| style == SCE_JAVA_INTERFACE
			|| style == SCE_JAVA_ENUM;
	case SCLEX_JAVASCRIPT:
		return style == SCE_JS_CLASS
			|| style == SCE_JS_INTERFACE
			|| style == SCE_JS_ENUM
			|| style == SCE_JS_WORD2;
	case SCLEX_KOTLIN:
		return style == SCE_KOTLIN_CLASS
			|| style == SCE_KOTLIN_INTERFACE
			|| style == SCE_KOTLIN_ENUM;
	case SCLEX_RUST:
		return style == SCE_RUST_TYPE
			|| style == SCE_RUST_STRUCT
			|| style == SCE_RUST_TRAIT
			|| style == SCE_RUST_ENUMERATION
			|| style == SCE_RUST_UNION;
	case SCLEX_SWIFT:
		return style == SCE_SWIFT_CLASS
			|| style == SCE_SWIFT_STRUCT
			|| style == SCE_SWIFT_PROTOCOL
			|| style == SCE_SWIFT_ENUM;
	default:
		return FALSE;
	}
}

void EditAutoCloseBraceQuote(int ch) {
	const Sci_Position iCurPos = SciCall_GetCurrentPos();
	const int chPrev = SciCall_GetCharAt(iCurPos - 2);
	const int chNext = SciCall_GetCharAt(iCurPos);
	const int iPrevStyle = SciCall_GetStyleAt(iCurPos - 2);
	const int iNextStyle = SciCall_GetStyleAt(iCurPos);

	const int charStyle = GetCharacterStyle(pLexCurrent->iLexer);
	if (charStyle != 0) {
		// within char
		if (iPrevStyle == charStyle && iNextStyle == charStyle && pLexCurrent->rid != NP2LEX_PHP) {
			return;
		}
	}

	// escape sequence
	if (ch != ',' && (chPrev == '\\' || (pLexCurrent->iLexer == SCLEX_BATCH && chPrev == '^'))) {
		return;
	}

	const int mask = autoCompletionConfig.fAutoInsertMask;
	char fillChar = '\0';
	BOOL closeBrace = FALSE;
	switch (ch) {
	case '(':
		if (mask & AutoInsertParenthesis) {
			fillChar = ')';
			closeBrace = TRUE;
		}
		break;
	case '[':
		if ((mask & AutoInsertSquareBracket) && !(pLexCurrent->rid == NP2LEX_SMALI)) { // JVM array type
			fillChar = ']';
			closeBrace = TRUE;
		}
		break;
	case '{':
		if (mask & AutoInsertBrace) {
			fillChar = '}';
			closeBrace = TRUE;
		}
		break;
	case '<':
		if ((mask & AutoInsertAngleBracket) && IsGenericTypeStyle(pLexCurrent->iLexer, iPrevStyle)) {
			// geriatric type, template
			fillChar = '>';
		}
		break;
	case '\"':
		if ((mask & AutoInsertDoubleQuote)) {
			fillChar = '\"';
		}
		break;
	case '\'':
		if ((mask & AutoInsertSingleQuote) && CanAutoCloseSingleQuote(chPrev, iPrevStyle)) {
			fillChar = '\'';
		}
		break;
	case '`':
		//if (pLexCurrent->iLexer == SCLEX_BASH
		//|| pLexCurrent->rid == NP2LEX_JULIA
		//|| pLexCurrent->iLexer == SCLEX_MAKEFILE
		//|| pLexCurrent->iLexer == SCLEX_SQL
		//) {
		//	fillChar = '`';
		//} else if (0) {
		//	fillChar = '\'';
		//}
		if (mask & AutoInsertBacktick) {
			fillChar = '`';
		}
		break;
	case ',':
		if ((mask & AutoInsertSpaceAfterComma) && !(chNext == ' ' || chNext == '\t' || (chPrev == '\'' && chNext == '\'') || (chPrev == '\"' && chNext == '\"'))) {
			fillChar = ' ';
		}
		break;
	default:
		break;
	}

	if (fillChar) {
		if (closeBrace && EditIsOpenBraceMatched(iCurPos - 1, iCurPos)) {
			return;
		}
		// TODO: auto escape quotes inside string
		//else if (ch == fillChar) {
		//}

		const char tchIns[4] = { fillChar };
		SciCall_BeginUndoAction();
		SciCall_ReplaceSel(tchIns);
		const Sci_Position iCurrentPos = (ch == ',') ? iCurPos + 1 : iCurPos;
		SciCall_SetSel(iCurrentPos, iCurrentPos);
		SciCall_EndUndoAction();
		if (closeBrace) {
			// fix brace matching
			SciCall_EnsureStyledTo(iCurPos + 1);
		}
	}
}

static inline BOOL IsHtmlVoidTag(const char *word, int length) {
	// see classifyTagHTML() in LexHTML.cxx
	const char *p = StrStrIA(
		// void elements
		" area base basefont br col command embed frame hr img input isindex keygen link meta param source track wbr "
		, word);
	return p != NULL && p[-1] == ' ' && p[length] == ' ';
}

void EditAutoCloseXMLTag(void) {
	char tchBuf[512];
	const Sci_Position iCurPos = SciCall_GetCurrentPos();
	const Sci_Position iStartPos = max_pos(0, iCurPos - (COUNTOF(tchBuf) - 1));
	const Sci_Position iSize = iCurPos - iStartPos;
	BOOL shouldAutoClose = iSize >= 3 && autoCompletionConfig.bCloseTags;
	BOOL autoClosed = FALSE;

	const int ignoreStyle = GetGenericOperatorStyle(pLexCurrent->iLexer);
	if (shouldAutoClose && ignoreStyle != 0) {
		int iCurrentStyle = SciCall_GetStyleAt(iCurPos);
		if (iCurrentStyle == 0 || iCurrentStyle == ignoreStyle) {
			shouldAutoClose = FALSE;
		} else if (pLexCurrent->iLexer == SCLEX_CPP) {
			const Sci_Line iLine = SciCall_LineFromPosition(iCurPos);
			Sci_Position iCurrentLinePos = SciCall_PositionFromLine(iLine);
			while (iCurrentLinePos < iCurPos && IsASpaceOrTab(SciCall_GetCharAt(iCurrentLinePos))) {
				iCurrentLinePos++;
			}
			iCurrentStyle = SciCall_GetStyleAt(iCurrentLinePos);
			if (SciCall_GetCharAt(iCurrentLinePos) == '#' && iCurrentStyle == SCE_C_PREPROCESSOR) {
				shouldAutoClose = FALSE;
			}
		}
	}

	if (shouldAutoClose) {
		struct Sci_TextRange tr = { { iStartPos, iCurPos }, tchBuf };
		SciCall_GetTextRange(&tr);

		if (tchBuf[iSize - 2] != '/') {
			char tchIns[516] = "</";
			int cchIns = 2;
			const char *pBegin = tchBuf;
			const char *pCur = tchBuf + iSize - 2;

			while (pCur > pBegin && *pCur != '<' && *pCur != '>') {
				--pCur;
			}

			if (*pCur == '<') {
				pCur++;
				while (IsHtmlTagChar(*pCur)) {
					tchIns[cchIns++] = *pCur;
					pCur++;
				}
			}

			tchIns[cchIns++] = '>';
			tchIns[cchIns] = '\0';

			shouldAutoClose = cchIns > 3;
			if (shouldAutoClose && pLexCurrent->iLexer == SCLEX_HTML) {
				tchIns[cchIns - 1] = '\0';
				shouldAutoClose = !IsHtmlVoidTag(tchIns + 2, cchIns - 3);
			}
			if (shouldAutoClose) {
				tchIns[cchIns - 1] = '>';
				autoClosed = TRUE;
				SciCall_BeginUndoAction();
				SciCall_ReplaceSel(tchIns);
				SciCall_SetSel(iCurPos, iCurPos);
				SciCall_EndUndoAction();
			}
		}
	}
	if (!autoClosed && autoCompletionConfig.bCompleteWord) {
		const Sci_Position iPos = SciCall_GetCurrentPos();
		if (SciCall_GetCharAt(iPos - 2) == '-') {
			EditCompleteWord(AutoCompleteCondition_Normal, FALSE); // obj->field, obj->method
		}
	}
}

BOOL IsIndentKeywordStyle(int style) {
	switch (pLexCurrent->iLexer) {
	//case SCLEX_AU3:
	//	return style == SCE_AU3_KEYWORD;
	case SCLEX_BASH:
		return style == SCE_BAT_WORD;

	case SCLEX_CMAKE:
		return style == SCE_CMAKE_WORD;
	//case SCLEX_CPP:
	//	return style == SCE_C_PREPROCESSOR;

	case SCLEX_JULIA:
		return style == SCE_JULIA_WORD;
	case SCLEX_LUA:
		return style == SCE_LUA_WORD;
	case SCLEX_MAKEFILE:
		return style == SCE_MAKE_PREPROCESSOR;
	case SCLEX_MATLAB:
		return style == SCE_MAT_KEYWORD;
	//case SCLEX_NSIS:
	//	return style == SCE_NSIS_WORD || style == SCE_NSIS_PREPROCESSOR;

	//case SCLEX_PASCAL:
	case SCLEX_RUBY:
		return style == SCE_RB_WORD;
	case SCLEX_SQL:
		return style == SCE_SQL_WORD;

	//case SCLEX_VB:
	//case SCLEX_VBSCRIPT:
	//	return style == SCE_B_KEYWORD;
	//case SCLEX_VERILOG:
	//	return style == SCE_V_WORD;
	//case SCLEX_VHDL:
	//	return style == SCE_VHDL_KEYWORD;
	}
	return FALSE;
}

const char *EditKeywordIndent(const char *head, int *indent) {
	char word[16] = "";
	char word_low[16] = "";
	int length = 0;
	const char *endPart = NULL;
	*indent = 0;

	while (*head && length < 15) {
		const char lower = *head | 0x20;
		if (lower < 'a' || lower > 'z') {
			break;
		}
		word[length] = *head;
		word_low[length] = lower;
		++length;
		++head;
	}

	switch (pLexCurrent->iLexer) {
	//case SCLEX_AU3:
	case SCLEX_BASH:
		if (np2LexLangIndex == IDM_LEXER_CSHELL) {
			if (StrEqualExA(word, "if")) {
				*indent = 2;
				endPart = "endif";
			} else if (StrEqualExA(word, "switch")) {
				*indent = 2;
				endPart = "endsw";
			} else if (StrEqualExA(word, "foreach") || StrEqualExA(word, "while")) {
				*indent = 2;
				endPart = "end";
			}
		} else {
			if (StrEqualExA(word, "if")) {
				*indent = 2;
				endPart = "fi";
			} else if (StrEqualExA(word, "case")) {
				*indent = 2;
				endPart = "esac";
			} else if (StrEqualExA(word, "do")) {
				*indent = 2;
				endPart = "done";
			}
		}
		break;

	case SCLEX_CMAKE:
		if (StrEqualExA(word, "function")) {
			*indent = 2;
			endPart = "endfunction()";
		} else if (StrEqualExA(word, "macro")) {
			*indent = 2;
			endPart = "endmacro()";
		} else if (StrEqualExA(word, "if")) {
			*indent = 2;
			endPart = "endif()";
		} else if (StrEqualExA(word, "foreach")) {
			*indent = 2;
			endPart = "endforeach()";
		} else if (StrEqualExA(word, "while")) {
			*indent = 2;
			endPart = "endwhile()";
		}
		break;
	//case SCLEX_CPP:

	//case SCLEX_INNOSETUP:

	case SCLEX_JULIA: {
		LPCSTR pKeywords = lexJulia.pKeyWords->pszKeyWords[1];
		LPCSTR p = strstr(pKeywords, word);
		if (p == pKeywords || (p != NULL &&  p[-1] == ' ')) {
			*indent = 2;
			endPart = "end";
		}
	} break;

	case SCLEX_LUA:
		if (StrEqualExA(word, "function") || StrEqualExA(word, "if") || StrEqualExA(word, "do")) {
			*indent = 2;
			endPart = "end";
		}
		break;

	case SCLEX_MAKEFILE:
		if (StrEqualExA(word, "if")) {
			*indent = 2;
			endPart = "endif";
		} else if (StrEqualExA(word, "define")) {
			*indent = 2;
			endPart = "endef";
		} else if (StrEqualExA(word, "for")) {
			*indent = 2;
			endPart = "endfor";
		}
		break;
	case SCLEX_MATLAB:
		if (StrEqualExA(word, "function")) {
			*indent = 1;
			// 'end' is optional
		} else if (StrEqualExA(word, "if") || StrEqualExA(word, "for") || StrEqualExA(word, "while") || StrEqualExA(word, "switch") || StrEqualExA(word, "try")) {
			*indent = 2;
			if (pLexCurrent->rid == NP2LEX_OCTAVE || np2LexLangIndex == IDM_LEXER_OCTAVE) {
				if (StrEqualExA(word, "if")) {
					endPart = "endif";
				} else if (StrEqualExA(word, "for")) {
					endPart = "endfor";
				} else if (StrEqualExA(word, "while")) {
					endPart = "endwhile";
				} else if (StrEqualExA(word, "switch")) {
					endPart = "endswitch";
				} else if (StrEqualExA(word, "try")) {
					endPart = "end_try_catch";
				}
			}
			if (endPart == NULL) {
				endPart = "end";
			}
		}
		break;

	//case SCLEX_NSIS:
	//case SCLEX_PASCAL:
	case SCLEX_RUBY:
		if (StrEqualExA(word, "if") || StrEqualExA(word, "do") || StrEqualExA(word, "while") || StrEqualExA(word, "for")) {
			*indent = 2;
			endPart = "end";
		}
		break;

	case SCLEX_SQL:
		if (StrEqualExA(word_low, "if")) {
			*indent = 2;
			endPart = "END IF;";
		} else if (StrEqualExA(word_low, "while")) {
			*indent = 2;
			endPart = "END WHILE;";
		} else if (StrEqualExA(word_low, "repeat")) {
			*indent = 2;
			endPart = "END REPEAT;";
		} else if (StrEqualExA(word_low, "loop") || StrEqualExA(word_low, "for")) {
			*indent = 2;
			endPart = "END LOOP;";
		} else if (StrEqualExA(word_low, "case")) {
			*indent = 2;
			endPart = "END CASE;";
		} else if (StrEqualExA(word_low, "begin")) {
			*indent = 2;
			if (StrStrIA(head, "transaction") != NULL) {
				endPart = "COMMIT;";
			} else {
				endPart = "END";
			}
		} else if (StrEqualExA(word_low, "start")) {
			if (StrStrIA(head, "transaction") != NULL) {
				*indent = 2;
				endPart = "COMMIT;";
			}
		}
		break;

	//case SCLEX_VB:
	//case SCLEX_VBSCRIPT:
	//case SCLEX_VERILOG:
	//case SCLEX_VHDL:
	}
	return endPart;
}

extern FILEVARS fvCurFile;

void EditAutoIndent(void) {
	Sci_Position iCurPos = SciCall_GetCurrentPos();
	//const Sci_Position iAnchorPos = SciCall_GetAnchor();
	const Sci_Line iCurLine = SciCall_LineFromPosition(iCurPos);
	//const Sci_Position iLineLength = SciCall_GetLineLength(iCurLine);
	//const Sci_Position iIndentBefore = SciCall_GetLineIndentation(iCurLine - 1);

	// Move bookmark along with line if inserting lines (pressing return at beginning of line) because Scintilla does not do this for us
	if (iCurLine > 0) {
		const Sci_Position iPrevLineLength = SciCall_GetLineEndPosition(iCurLine - 1) - SciCall_PositionFromLine(iCurLine - 1);
		if (iPrevLineLength == 0) {
			const Sci_MarkerMask bitmask = SciCall_MarkerGet(iCurLine - 1);
			if (bitmask & MarkerBitmask_Bookmark) {
				SciCall_MarkerDelete(iCurLine - 1, MarkerNumber_Bookmark);
				SciCall_MarkerAdd(iCurLine, MarkerNumber_Bookmark);
			}
		}
	}

	if (iCurLine > 0/* && iLineLength <= 2*/) {
		const Sci_Position iPrevLineLength = SciCall_GetLineLength(iCurLine - 1);
		if (iPrevLineLength < 2) {
			return;
		}
		char *pLineBuf = (char *)NP2HeapAlloc(2 * iPrevLineLength + 1 + fvCurFile.iIndentWidth * 2 + 2 + 64);
		if (pLineBuf == NULL) {
			return;
		}

		const int iEOLMode = SciCall_GetEOLMode();
		int indent = 0;
		Sci_Position iIndentLen = 0;
		int commentStyle = 0;
		SciCall_GetLine(iCurLine - 1, pLineBuf);
		pLineBuf[iPrevLineLength] = '\0';

		int ch = (uint8_t)pLineBuf[iPrevLineLength - 2];
		if (ch == '\r') {
			ch = (uint8_t)pLineBuf[iPrevLineLength - 3];
			iIndentLen = 1;
		}
		if (ch == '{' || ch == '[' || ch == '(') {
			indent = 2;
		} else if (ch == ':') { // case label/Python
			indent = 1;
		} else if (ch == '*' || ch == '!') { // indent block comment
			iIndentLen = iPrevLineLength - (2 + iIndentLen);
			if (iIndentLen >= 2 && pLineBuf[iIndentLen - 2] == '/' && pLineBuf[iIndentLen - 1] == '*') {
				indent = 1;
				commentStyle = 1;
			}
		}

		iIndentLen = 0;
		ch = SciCall_GetCharAt(SciCall_PositionFromLine(iCurLine));
		const BOOL closeBrace = (ch == '}' || ch == ']' || ch == ')');
		if (indent == 2 && !closeBrace) {
			indent = 1;
		}

		char *pPos;
		const char *endPart = NULL;
		for (pPos = pLineBuf; *pPos; pPos++) {
			if (*pPos != ' ' && *pPos != '\t') {
				if (!indent && IsAlpha(*pPos)) { // indent on keywords
					const int style = SciCall_GetStyleAt(SciCall_PositionFromLine(iCurLine - 1) + iIndentLen);
					if (IsIndentKeywordStyle(style)) {
						endPart = EditKeywordIndent(pPos, &indent);
					}
				}
				if (indent) {
					ZeroMemory(pPos, iPrevLineLength - iIndentLen);
				}
				*pPos = '\0';
				break;
			}
			iIndentLen += 1;
		}

		if (indent == 2 && endPart) {
			const int level = SciCall_GetFoldLevel(iCurLine);
			if (!(level & SC_FOLDLEVELHEADERFLAG)) {
				const Sci_Line parent = SciCall_GetFoldParent(iCurLine);
				if (parent >= 0 && parent + 1 == iCurLine) {
					const Sci_Line child = SciCall_GetLastChild(parent);
					// TODO: check endPart is on this line
					if (SciCall_GetLineLength(child)) {
						indent = 1;
					}
				} else {
					indent = 0;
				}
			}
		}

		Sci_Position iIndentPos = iCurPos;
		if (indent) {
			int pad = fvCurFile.iIndentWidth;
			iIndentPos += iIndentLen;
			ch = ' ';
			if (fvCurFile.bTabIndents) {
				if (fvCurFile.bTabsAsSpaces) {
					pad = fvCurFile.iTabWidth;
					ch = ' ';
				} else {
					pad = 1;
					ch = '\t';
				}
			}
			if (commentStyle) {
				iIndentPos += 2;
				*pPos++ = ' ';
				*pPos++ = '*';
			} else {
				iIndentPos += pad;
				while (pad-- > 0) {
					*pPos++ = (char)ch;
				}
			}
			if (indent == 2) {
				switch (iEOLMode) {
				default: // SC_EOL_CRLF
					*pPos++ = '\r';
					*pPos++ = '\n';
					break;
				case SC_EOL_LF:
					*pPos++ = '\n';
					break;
				case SC_EOL_CR:
					*pPos++ = '\r';
					break;
				}
				strncpy(pPos, pLineBuf, iIndentLen + 1);
				pPos += iIndentLen;
				if (endPart) {
					iIndentLen = strlen(endPart);
					memcpy(pPos, endPart, iIndentLen);
					pPos += iIndentLen;
				}
			}
			*pPos = '\0';
		}

		if (*pLineBuf) {
			SciCall_BeginUndoAction();
			SciCall_AddText(strlen(pLineBuf), pLineBuf);
			if (indent) {
				SciCall_SetSel(iIndentPos, iIndentPos);
			}
			SciCall_EndUndoAction();

			//const Sci_Position iPrevLineStartPos = SciCall_PositionFromLine(iCurLine - 1);
			//const Sci_Position iPrevLineEndPos = SciCall_GetLineEndPosition(iCurLine - 1);
			//const Sci_Position iPrevLineIndentPos = SciCall_GetLineIndentPosition(iCurLine - 1);

			//if (iPrevLineEndPos == iPrevLineIndentPos) {
			//	SciCall_BeginUndoAction();
			//	SciCall_SetTargetRange(iPrevLineStartPos, iPrevLineEndPos);
			//	SciCall_ReplaceTarget(0, "");
			//	SciCall_EndUndoAction();
			//}
		}

		NP2HeapFree(pLineBuf);
		//const Sci_Position iIndent = SciCall_GetLineIndentation(iCurLine);
		//SciCall_SetLineIndentation(iCurLine, iIndentBefore);
		//iIndentLen = SciCall_GetLineIndentation(iCurLine);
		//if (iIndentLen > 0)
		//	SciCall_SetSel(iAnchorPos + iIndentLen, iCurPos + iIndentLen);
	}
}

void EditToggleCommentLine(void) {
	switch (pLexCurrent->iLexer) {
	case SCLEX_APDL:
		EditToggleLineComments((pLexCurrent->rid == NP2LEX_APDL) ? L"!" : L"**", FALSE);
		break;

	case SCLEX_ASM: {
		LPCWSTR ch;
		switch (autoCompletionConfig.iAsmLineCommentChar) {
		case AsmLineCommentCharSemicolon:
		default:
			ch = L";";
			break;
		case AsmLineCommentCharSharp:
			ch = L"# ";
			break;
		case AsmLineCommentCharSlash:
			ch = L"//";
			break;
		case AsmLineCommentCharAt:
			ch = L"@ ";
			break;
		}
		EditToggleLineComments(ch, FALSE);
	}
	break;

	case SCLEX_AHK:
	case SCLEX_AU3:
	case SCLEX_LISP:
	case SCLEX_LLVM:
	case SCLEX_PROPERTIES:
	case SCLEX_REBOL:
		EditToggleLineComments(L";", FALSE);
		break;

	case SCLEX_BASH:
		EditToggleLineComments(((np2LexLangIndex == IDM_LEXER_M4)? L"dnl " : L"#"), FALSE);
		break;

	case SCLEX_BATCH:
		EditToggleLineComments(L"@rem ", TRUE);
		break;

	case SCLEX_ASYMPTOTE:
	case SCLEX_CIL:
	case SCLEX_CPP:
	case SCLEX_CSHARP:
	case SCLEX_CSS: // for SCSS, LESS, HSS
	case SCLEX_D:
	case SCLEX_DART:
	case SCLEX_FSHARP:
	case SCLEX_GO:
	case SCLEX_GRAPHVIZ:
	case SCLEX_GROOVY:
	case SCLEX_HAXE:
	case SCLEX_JAVA:
	case SCLEX_JAVASCRIPT:
	case SCLEX_JSON:
	case SCLEX_KOTLIN:
	case SCLEX_PASCAL:
	case SCLEX_RUST:
	case SCLEX_SWIFT:
	case SCLEX_VERILOG:
		EditToggleLineComments(L"//", FALSE);
		break;

	case SCLEX_AVS:
	case SCLEX_AWK:
	case SCLEX_CMAKE:
	case SCLEX_COFFEESCRIPT:
	case SCLEX_CONF:
	case SCLEX_GN:
	case SCLEX_JAM:
	case SCLEX_JULIA:
	case SCLEX_MAKEFILE:
	case SCLEX_NSIS:
	case SCLEX_PERL:
	case SCLEX_POWERSHELL:
	case SCLEX_PYTHON:
	case SCLEX_R:
	case SCLEX_RUBY:
	case SCLEX_SMALI:
	case SCLEX_TCL:
	case SCLEX_TOML:
	case SCLEX_YAML:
		EditToggleLineComments(L"#", FALSE);
		break;

	case SCLEX_FORTRAN:
		EditToggleLineComments(L"!", FALSE);
		break;

	case SCLEX_HTML:
	case SCLEX_XML: {
		const int block = GetCurrentHtmlTextBlock();
		switch (block) {
		case HTML_TEXT_BLOCK_VBS:
			EditToggleLineComments(L"'", FALSE);
			break;

		case HTML_TEXT_BLOCK_PYTHON:
			EditToggleLineComments(L"#", FALSE);
			break;

		case HTML_TEXT_BLOCK_CDATA:
		case HTML_TEXT_BLOCK_JS:
		case HTML_TEXT_BLOCK_PHP:
			EditToggleLineComments(L"//", FALSE);
			break;
		}
	}
	break;

	case SCLEX_INNOSETUP: {
		const int lineState = SciCall_GetLineState(SciCall_LineFromPosition(SciCall_GetSelectionStart()));
		if (lineState & InnoLineStateCodeSection) {
			EditToggleLineComments(L"//", FALSE);
		} else {
			EditToggleLineComments(L";", FALSE);
		}
	}
	break;

	case SCLEX_LATEX:
		EditToggleLineComments(L"%", FALSE);
		break;

	case SCLEX_LUA:
	case SCLEX_VHDL:
		EditToggleLineComments(L"--", FALSE);
		break;

	case SCLEX_MATLAB:
		if (pLexCurrent->rid == NP2LEX_SCILAB || np2LexLangIndex == IDM_LEXER_SCILAB) {
			EditToggleLineComments(L"//", FALSE);
		} else {
			EditToggleLineComments(L"%", FALSE);
		}
		break;

	case SCLEX_SQL:
		EditToggleLineComments(L"-- ", FALSE); // extra space
		break;

	case SCLEX_TEXINFO:
		EditToggleLineComments(L"@c ", FALSE);
		break;

	case SCLEX_VB:
	case SCLEX_VBSCRIPT:
		EditToggleLineComments(L"'", FALSE);
		break;

	case SCLEX_VIM:
		EditToggleLineComments(L"\"", FALSE);
		break;

	case SCLEX_WASM:
		EditToggleLineComments(L";;", FALSE);
		break;
	}
}

void EditEncloseSelectionNewLine(LPCWSTR pwszOpen, LPCWSTR pwszClose) {
	WCHAR start[64] = L"";
	WCHAR end[64] = L"";
	const int iEOLMode = SciCall_GetEOLMode();
	LPCWSTR lineEnd = (iEOLMode == SC_EOL_LF) ? L"\n" : ((iEOLMode == SC_EOL_CR) ? L"\r" : L"\r\n");

	Sci_Position pos = SciCall_GetSelectionStart();
	Sci_Line line = SciCall_LineFromPosition(pos);
	if (pos != SciCall_PositionFromLine(line)) {
		lstrcat(start, lineEnd);
	}
	lstrcat(start, pwszOpen);
	lstrcat(start, lineEnd);

	pos = SciCall_GetSelectionEnd();
	line = SciCall_LineFromPosition(pos);
	if (pos != SciCall_PositionFromLine(line)) {
		lstrcat(end, lineEnd);
	}
	lstrcat(end, pwszClose);
	lstrcat(end, lineEnd);
	EditEncloseSelection(start, end);
}

void EditToggleCommentBlock(void) {
	switch (pLexCurrent->iLexer) {
	case SCLEX_AHK:
	case SCLEX_ASM:
	case SCLEX_ASYMPTOTE:
	case SCLEX_AVS:
	case SCLEX_CIL:
	case SCLEX_CPP:
	case SCLEX_CSHARP:
	case SCLEX_CSS:
	case SCLEX_D:
	case SCLEX_DART:
	case SCLEX_GO:
	case SCLEX_GROOVY:
	case SCLEX_HAXE:
	case SCLEX_JAVA:
	case SCLEX_JAVASCRIPT:
	case SCLEX_JSON:
	case SCLEX_KOTLIN:
	case SCLEX_NSIS:
	case SCLEX_RUST:
	case SCLEX_SQL:
	case SCLEX_SWIFT:
	case SCLEX_VERILOG:
	case SCLEX_VHDL:
		EditEncloseSelection(L"/*", L"*/");
		break;

	case SCLEX_AU3:
		EditEncloseSelectionNewLine(L"#cs", L"#ce");
		break;

	case SCLEX_CMAKE:
		EditEncloseSelection(L"#[[", L"]]");
		break;

	case SCLEX_COFFEESCRIPT:
		EditEncloseSelection(L"###", L"###");
		break;

	case SCLEX_FORTRAN:
		EditEncloseSelectionNewLine(L"#if 0", L"#endif");
		break;

	case SCLEX_FSHARP:
		EditEncloseSelection(L"(*", L"*)");
		break;

	case SCLEX_GRAPHVIZ: {
		const int lineState = SciCall_GetLineState(SciCall_LineFromPosition(SciCall_GetSelectionStart()));
		if (lineState) {
			EditEncloseSelection(L"<!--", L"-->");
		} else {
			EditEncloseSelection(L"/*", L"*/");
		}
	} break;

	case SCLEX_HTML:
	case SCLEX_XML: {
		const int block = GetCurrentHtmlTextBlock();
		switch (block) {
		case HTML_TEXT_BLOCK_TAG:
			EditEncloseSelection(L"<!--", L"-->");
			break;

		case HTML_TEXT_BLOCK_CDATA:
		case HTML_TEXT_BLOCK_JS:
		case HTML_TEXT_BLOCK_PHP:
		case HTML_TEXT_BLOCK_CSS:
			EditEncloseSelection(L"/*", L"*/");
			break;

		case HTML_TEXT_BLOCK_SGML:
			// A brief SGML tutorial
			// https://www.w3.org/TR/WD-html40-970708/intro/sgmltut.html
			EditEncloseSelection(L"--", L"--");
			break;
		}
	}
	break;

	case SCLEX_INNOSETUP: {
		const int lineState = SciCall_GetLineState(SciCall_LineFromPosition(SciCall_GetSelectionStart()));
		if (lineState &  InnoLineStateCodeSection) {
			EditEncloseSelection(L"{", L"}");
		} else if (lineState & InnoLineStatePreprocessor) {
			EditEncloseSelection(L"/*", L"*/");
		}
	}
	break;

	case SCLEX_PASCAL:
		EditEncloseSelection(L"{", L"}");
		break;

	case SCLEX_JAM:
	case SCLEX_LISP:
		EditEncloseSelection(L"#|", L"|#");
		break;

	case SCLEX_JULIA:
		EditEncloseSelection(L"#=", L"=#");
		break;

	case SCLEX_LATEX:
		EditEncloseSelectionNewLine(L"\\begin{comment}", L"\\end{comment}");
		break;

	case SCLEX_LUA:
		EditEncloseSelection(L"--[[", L"--]]");
		break;

	case SCLEX_MARKDOWN:
		EditEncloseSelection(L"<!--", L"-->");
		break;

	case SCLEX_MATLAB:
		if (pLexCurrent->rid == NP2LEX_SCILAB || np2LexLangIndex == IDM_LEXER_SCILAB) {
			EditEncloseSelection(L"/*", L"*/");
		} else {
			EditEncloseSelectionNewLine(L"%{", L"%}");
		}
		break;

	case SCLEX_POWERSHELL:
		EditEncloseSelection(L"<#", L"#>");
		break;

	case SCLEX_R:
		EditEncloseSelectionNewLine(L"if (FALSE) {", L"}");
		break;

	case SCLEX_REBOL:
		EditEncloseSelectionNewLine(L"comment {", L"}");
		break;

	case SCLEX_TCL:
		EditEncloseSelectionNewLine(L"if (0) {", L"}");
		break;

	case SCLEX_WASM:
		EditEncloseSelection(L"(;", L";)");
		break;
	}
}

// see Style_SniffShebang() in Styles.c
void EditInsertScriptShebangLine(void) {
	const char *prefix = "#!/usr/bin/env ";
	const char *name = NULL;

	switch (pLexCurrent->iLexer) {
	case SCLEX_BASH:
		switch (np2LexLangIndex) {
		case IDM_LEXER_CSHELL:
			prefix = "#!/bin/csh";
			break;

		case IDM_LEXER_M4:
			name = "m4";
			break;

		default:
			prefix = "#!/bin/bash";
			break;
		}
		break;

	case SCLEX_CPP:
		switch (pLexCurrent->rid) {
		case NP2LEX_AWK:
			name = "awk";
			break;

		case NP2LEX_GROOVY:
			name = "groovy";
			break;

		case NP2LEX_JAVASCRIPT:
			name = "node";
			break;

		case NP2LEX_PHP:
			name = "php";
			break;

		case NP2LEX_SCALA:
			name = "scala";
			break;
		}
		break;

	//case SCLEX_KOTLIN:
	//	name = "kotlin";
	//	break;

	case SCLEX_LUA:
		name = "lua";
		break;

	case SCLEX_PERL:
		name = "perl";
		break;

	case SCLEX_PYTHON:
		name = "python3";
		break;

	case SCLEX_RUBY:
		name = "ruby";
		break;

	//case SCLEX_RUST:
	//	name = "rust";
	//	break;

	case SCLEX_TCL:
		name = "wish";
		break;
	}

	char line[128];
	strcpy(line, prefix);
	if (name != NULL) {
		strcat(line, name);
	}

	const Sci_Position iCurrentPos = SciCall_GetCurrentPos();
	if (iCurrentPos == 0 && (name != NULL || pLexCurrent->iLexer == SCLEX_BASH)) {
		const int iEOLMode = SciCall_GetEOLMode();
		LPCSTR lineEnd = (iEOLMode == SC_EOL_LF) ? "\n" : ((iEOLMode == SC_EOL_CR) ? "\r" : "\r\n");
		strcat(line, lineEnd);
	}
	SciCall_ReplaceSel(line);
}

void EditShowCallTips(Sci_Position position) {
	const Sci_Line iLine = SciCall_LineFromPosition(position);
	const Sci_Position iDocLen = SciCall_GetLineLength(iLine);
	char *pLine = (char *)NP2HeapAlloc(iDocLen + 1);
	SciCall_GetLine(iLine, pLine);
	StrTrimA(pLine, " \t\r\n");
	char *text = (char *)NP2HeapAlloc(iDocLen + 1 + 128);
#if defined(_WIN64)
	sprintf(text, "ShowCallTips(%" PRId64 ", %" PRId64 ", %" PRId64 ")\n\n\002%s", iLine + 1, position, iDocLen, pLine);
#else
	sprintf(text, "ShowCallTips(%d, %d, %d)\n\n\002%s", (int)(iLine + 1), (int)position, (int)iDocLen, pLine);
#endif
	SciCall_CallTipUseStyle(fvCurFile.iTabWidth);
	SciCall_CallTipShow(position, text);
	NP2HeapFree(pLine);
	NP2HeapFree(text);
}
