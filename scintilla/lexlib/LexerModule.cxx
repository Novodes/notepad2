// Scintilla source code edit control
/** @file LexerModule.cxx
 ** Colourise for particular languages.
 **/
// Copyright 1998-2010 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstdlib>
#include <cassert>

#include <string_view>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "PropSetSimple.h"
#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "LexerModule.h"
#include "LexerBase.h"

using namespace Lexilla;

//++Autogenerated -- run scripts/LexGen.py to regenerate
//**\(extern LexerModule \*;\n\)
extern LexerModule lmAHK;
extern LexerModule lmAPDL;
extern LexerModule lmAsm;
extern LexerModule lmAsymptote;
extern LexerModule lmAU3;
extern LexerModule lmAVS;
extern LexerModule lmAwk;
extern LexerModule lmBash;
extern LexerModule lmBatch;
extern LexerModule lmCIL;
extern LexerModule lmCMake;
extern LexerModule lmCoffeeScript;
extern LexerModule lmConf;
extern LexerModule lmCPP;
extern LexerModule lmCSharp;
extern LexerModule lmCss;
extern LexerModule lmD;
extern LexerModule lmDart;
extern LexerModule lmDiff;
extern LexerModule lmFortran;
extern LexerModule lmFSharp;
extern LexerModule lmGN;
extern LexerModule lmGo;
extern LexerModule lmGraphViz;
extern LexerModule lmGroovy;
extern LexerModule lmHaxe;
extern LexerModule lmHTML;
extern LexerModule lmInno;
extern LexerModule lmJam;
extern LexerModule lmJava;
extern LexerModule lmJavaScript;
extern LexerModule lmJSON;
extern LexerModule lmJulia;
extern LexerModule lmKotlin;
extern LexerModule lmLatex;
extern LexerModule lmLisp;
extern LexerModule lmLLVM;
extern LexerModule lmLua;
extern LexerModule lmMake;
extern LexerModule lmMarkdown;
extern LexerModule lmMatlab;
extern LexerModule lmNsis;
extern LexerModule lmNull;
extern LexerModule lmPascal;
extern LexerModule lmPerl;
extern LexerModule lmPowerShell;
extern LexerModule lmProps;
extern LexerModule lmPython;
extern LexerModule lmR;
extern LexerModule lmREBOL;
extern LexerModule lmRuby;
extern LexerModule lmRust;
extern LexerModule lmSmali;
extern LexerModule lmSQL;
extern LexerModule lmSwift;
extern LexerModule lmTCL;
extern LexerModule lmTexinfo;
extern LexerModule lmTOML;
extern LexerModule lmVB;
extern LexerModule lmVBScript;
extern LexerModule lmVerilog;
extern LexerModule lmVHDL;
extern LexerModule lmVim;
extern LexerModule lmWASM;
extern LexerModule lmXML;
extern LexerModule lmYAML;

//--Autogenerated -- end of automatically generated section

namespace {

const LexerModule * const lexerCatalogue[] = {
//++Autogenerated -- run scripts/LexGen.py to regenerate
//**\(\t&\*,\n\)
	&lmAHK,
	&lmAPDL,
	&lmAsm,
	&lmAsymptote,
	&lmAU3,
	&lmAVS,
	&lmAwk,
	&lmBash,
	&lmBatch,
	&lmCIL,
	&lmCMake,
	&lmCoffeeScript,
	&lmConf,
	&lmCPP,
	&lmCSharp,
	&lmCss,
	&lmD,
	&lmDart,
	&lmDiff,
	&lmFortran,
	&lmFSharp,
	&lmGN,
	&lmGo,
	&lmGraphViz,
	&lmGroovy,
	&lmHaxe,
	&lmHTML,
	&lmInno,
	&lmJam,
	&lmJava,
	&lmJavaScript,
	&lmJSON,
	&lmJulia,
	&lmKotlin,
	&lmLatex,
	&lmLisp,
	&lmLLVM,
	&lmLua,
	&lmMake,
	&lmMarkdown,
	&lmMatlab,
	&lmNsis,
	&lmNull,
	&lmPascal,
	&lmPerl,
	&lmPowerShell,
	&lmProps,
	&lmPython,
	&lmR,
	&lmREBOL,
	&lmRuby,
	&lmRust,
	&lmSmali,
	&lmSQL,
	&lmSwift,
	&lmTCL,
	&lmTexinfo,
	&lmTOML,
	&lmVB,
	&lmVBScript,
	&lmVerilog,
	&lmVHDL,
	&lmVim,
	&lmWASM,
	&lmXML,
	&lmYAML,

//--Autogenerated -- end of automatically generated section
};

}

const LexerModule *LexerModule::Find(int language_) noexcept {
	for (const LexerModule *lm : lexerCatalogue) {
		if (lm->GetLanguage() == language_) {
			return lm;
		}
	}
	return &lmNull;
}

Scintilla::ILexer5 *LexerModule::Create() const {
	if (fnFactory) {
		return fnFactory();
	}
	return new LexerBase(this);
}
