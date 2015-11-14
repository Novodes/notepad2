#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "CharacterSet.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "LexerModule.h"

static inline bool IsTexiSpec(int ch) {
	return ch == '@' || ch == '{' || ch == '}' ||
		ch == '*' || ch == '/' || ch == '-' ||
		ch == ':' || ch == '.' || ch == '?' || ch == '?' ||
		ch == '\"' || ch == '\'' || ch == ',' || ch == '=' || ch == '~';
}

#define MAX_WORD_LENGTH	31
static void ColouriseTexiDoc(Sci_PositionU startPos, Sci_Position length, int initStyle, WordList *keywordLists[], Accessor &styler) {
	const bool fold = styler.GetPropertyInt("fold", 1) != 0;
	//WordList &keywords = *keywordLists[0]; // command
	WordList &keywords2 = *keywordLists[1];// fold
	//WordList &keywords3 = *keywordLists[2];// condition
	//WordList &keywords4 = *keywordLists[3];// command with arg

	int state = initStyle;
	int ch = 0, chNext = styler[startPos];
	styler.StartAt(startPos);
	styler.StartSegment(startPos);
	Sci_PositionU endPos = startPos + length;
	if (endPos == (Sci_PositionU)styler.Length())
		++endPos;

	Sci_Position lineCurrent = styler.GetLine(startPos);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0)
		levelCurrent = styler.LevelAt(lineCurrent-1) >> 16;
	int levelNext = levelCurrent;
	char buf[MAX_WORD_LENGTH + 1] = {0};
	int wordLen = 0;
	bool isCommand = false;

	for (Sci_PositionU i = startPos; i < endPos; i++) {
		const int chPrev = ch;
		ch = chNext;
		chNext = styler.SafeGetCharAt(i + 1);

		const bool atEOL = (ch == '\r' && chNext != '\n') || (ch == '\n');
		const bool atLineStart = i == (Sci_PositionU)styler.LineStart(lineCurrent);

		switch (state) {
		case SCE_L_OPERATOR:
			styler.ColourTo(i - 1, state);
			state = SCE_L_DEFAULT;
			break;
		case SCE_L_SPECIAL:
			styler.ColourTo(i, state);
			state = SCE_L_DEFAULT;
			continue;
		case SCE_L_COMMAND:
			if (!IsAlpha(ch)) {
				buf[wordLen] = 0;
				if (strcmp(buf, "@c") == 0 || strcmp(buf, "@comment") == 0) {
					state = SCE_L_COMMENT;
				} else if (strcmp(buf, "@end") == 0) {
					levelNext--;
					isCommand = true;
				} else {
					if (buf[0] == '@' && keywords2.InList(&buf[1])) {
						levelNext++;
					}
					if (strcmp(buf, "@settitle") == 0) {
						state = SCE_L_TITLE;
					} else if (strcmp(buf, "@chapter") == 0) {
						state = SCE_L_CHAPTER;
					} else if (strcmp(buf, "@section") == 0) {
						state = SCE_L_SECTION;
					} else if (strcmp(buf, "@subsection") == 0) {
						state = SCE_L_SECTION1;
					} else if (strcmp(buf, "@subsubsection") == 0) {
						state = SCE_L_SECTION2;
					}
				}
				if (state == SCE_L_COMMAND) {
					styler.ColourTo(i - 1, state);
					state = SCE_L_DEFAULT;
				}
				wordLen = 0;
			} else if (wordLen < MAX_WORD_LENGTH) {
				buf[wordLen++] = (char)ch;
			}
			break;
		case SCE_L_TAG:
			if (!IsLowerCase(ch)) {
				if (isCommand) {
					styler.ColourTo(i - 1, SCE_L_COMMAND);
				}
				state = SCE_L_DEFAULT;
			}
			break;
		case SCE_L_COMMENT:
		case SCE_L_TITLE:
		case SCE_L_CHAPTER:
		case SCE_L_SECTION:
		case SCE_L_SECTION1:
		case SCE_L_SECTION2:
			if (atLineStart) {
				styler.ColourTo(i - 1, state);
				state = SCE_L_DEFAULT;
			}
			break;
		}

		if (state == SCE_L_DEFAULT) {
			if (lineCurrent == 0 && i == 0 && ch == '\\' && chNext == 'i') { // \input texinfo.tex
				state = SCE_L_COMMAND;
				buf[wordLen++] = (char)ch;
			} else if (ch == '@') {
				if (IsTexiSpec(chNext)) {
					styler.ColourTo(i - 1, state);
					state = SCE_L_SPECIAL;
				} else if (IsAlpha(chNext)) {
					styler.ColourTo(i - 1, state);
					state = SCE_L_COMMAND;
					buf[wordLen++] = (char)ch;
				}
			} else if (ch == '@' || ch == '{' || ch == '}' ||
				(ch == '-' && !IsAlpha(chPrev) && !IsAlpha(chNext))) {
				styler.ColourTo(i - 1, state);
				state = SCE_L_OPERATOR;
			} else if (IsLowerCase(ch)) {
				styler.ColourTo(i - 1, state);
				state = SCE_L_TAG;
			}
		}

		if (atEOL || i == endPos-1) {
			if (fold) {
				int levelUse = levelCurrent;
				int lev = levelUse | levelNext << 16;
				if (levelUse < levelNext)
					lev |= SC_FOLDLEVELHEADERFLAG;
				if (lev != styler.LevelAt(lineCurrent)) {
					styler.SetLevel(lineCurrent, lev);
				}
				levelCurrent = levelNext;
			}
			lineCurrent++;
			isCommand = false;
		}

	}

	// Colourise remaining document
	styler.ColourTo(endPos - 1, state);
}

LexerModule lmTexinfo(SCLEX_TEXINFO, ColouriseTexiDoc, "texi");
