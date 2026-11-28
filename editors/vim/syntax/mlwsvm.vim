" Vim syntax file
" Language:    MLWSVM assembly (.masm)
" Maintainer:  https://github.com/msharpe248/mlws-vm (editors/vim)
" License:     MIT
"
" The same elements as the VS Code grammar (editors/vscode): directives,
" instructions and their type suffixes (after a label too), labels and
" 1f/1b, registers, strings and their escapes, characters, numbers with
" BigInt and decimal literals, macro parameters, R0->Struct.field, a
" signature's types and names, a .struct's fields, and a test's
" ; expect-stdout: lines.

if exists('b:current_syntax')
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match
syn iskeyword @,48-57,_,$

" ---- comments --------------------------------------------------------------

syn keyword mlwsvmTodo      contained TODO FIXME XXX NOTE
" A test's header: ; expect-stdout: ..., ; stdin: ..., ; args: ...
syn match   mlwsvmTestKey   contained /\%(^;\s*\)\@<=\%(expect-\l\+\|stdin\|args\|skip\|threads\|option\|requires\|debug-script\|no-opt\):/
syn match   mlwsvmComment   /;.*$/ contains=mlwsvmTestKey,mlwsvmTodo,@Spell

" ---- literals and operands -------------------------------------------------

syn match   mlwsvmEscape    contained /\\\%([nrt0\\"'abfv]\|x\x\x\|u{\x\+}\|u\x\{4}\)/
syn region  mlwsvmString    start=/"/ skip=/\\\\\|\\"/ end=/"/ oneline contains=mlwsvmEscape
syn match   mlwsvmChar      /'\%([^'\\]\|\\\%([nrt0\\"'abfv]\|x\x\x\|u{\x\+}\|u\x\{4}\)\)'/

syn match   mlwsvmNumber    /-\=\<\d[0-9_]*\%(\.\d\+\)\=\%([eE][+-]\=\d\+\)\=\>/
syn match   mlwsvmNumber    /-\=\<0[xX][0-9A-Fa-f_]\+\>/
syn match   mlwsvmNumber    /-\=\<0[bB][01_]\+\>/
syn match   mlwsvmNumber    /-\=\<0[oO][0-7_]\+\>/
" BigInt (123n) and decimal (19.99d)
syn match   mlwsvmNumber    /-\=\<\d[0-9_]*\%(\.\d\+\)\=\%([eE][+-]\=\d\+\)\=[nd]\>/
syn keyword mlwsvmConstant  null true false inf nan

" Local label references: 1f (forward), 1b (back)
syn match   mlwsvmLabelRef  /\<\d\+[fb]\>/

syn match   mlwsvmRegister  /\<\%([IFRP]\%(\d\|[1-9]\d\)\|SP\|FP\|PID\)\>/
syn match   mlwsvmRegister  /\<\%(UP\|ARG\|LOC\)\ze\s*\[/

" A macro's parameter (\name), \@ and \()
syn match   mlwsvmMacroArg  /\\\%(\h\w*\|@\|()\)/

syn match   mlwsvmArrow     /->/
" R0->Struct.field
syn match   mlwsvmStructRef /\%(->\)\@<=[A-Za-z_$][0-9A-Za-z_$]*\ze\./ nextgroup=mlwsvmFieldDot
syn match   mlwsvmFieldDot  contained /\./ nextgroup=mlwsvmField
syn match   mlwsvmField     contained /[A-Za-z_$][0-9A-Za-z_$]*/

syn cluster mlwsvmOperands  contains=mlwsvmString,mlwsvmChar,mlwsvmNumber,mlwsvmConstant,mlwsvmLabelRef,mlwsvmRegister,mlwsvmMacroArg,mlwsvmArrow,mlwsvmStructRef,mlwsvmComment

" ---- directives -------------------------------------------------------------

syn match   mlwsvmDirective /\%(^\|[^0-9A-Za-z_.$\\]\)\@<=\.\%(alias\|align\|ascii\|asciz\|b\|c\|callback\|const\|cstruct\|debug_format\|elif\|else\|endf\|endif\|endm\|endr\|ends\|endtry\|equ\|error\|export\|extension\|extern\|f32\|f64\|ffi_free\|func\|i8\|i16\|i32\|i64\|if\|ifb\|ifdef\|ifnb\|ifndef\|import\|include\|irp\|library\|loc\|local\|macro\|module\|public\|regs\|reload\|rept\|requires\|set\|space\|struct\|try\|u8\|u16\|u32\|u64\|zero\)\>/

" ---- labels and instructions ------------------------------------------------

" The first word on a line is a mnemonic (or a macro), with its type suffixes.
syn match   mlwsvmSuffix    contained /\%(\.\l[a-z0-9]*\)\+/
syn match   mlwsvmInsn      /^\s*\zs\h\w*\ze\%(\.\l[a-z0-9]*\)*\%(\s\|;\|$\)/ nextgroup=mlwsvmSuffix
syn match   mlwsvmInsnAfter contained /\h\w*\ze\%(\.\l[a-z0-9]*\)*\%(\s\|;\|$\)/ nextgroup=mlwsvmSuffix

" Labels: name:, 1:, .Lloop\@:, and several on a line (1: 2: nop)
let s:label = '\%([A-Za-z_.$]\|\\@\|\\()\|\\\h\w*\)\%([0-9A-Za-z_.$]\|\\@\|\\()\|\\\h\w*\)*\|\d\+'
exe 'syn match mlwsvmLabel /^\s*\zs\%(' . s:label . '\)\ze:\%(:\)\@!/ nextgroup=mlwsvmLabelColon'
exe 'syn match mlwsvmLabelNext contained /\%(' . s:label . '\)\ze:\%(:\)\@!/ nextgroup=mlwsvmLabelColon'
syn match   mlwsvmLabelColon contained /:/ nextgroup=mlwsvmLabelNext,mlwsvmInsnAfter skipwhite
unlet s:label

" ---- signatures -------------------------------------------------------------

" .func name(i64 a, ref b) -> i64
" .extern c.strlen, "", "strlen" (cstr) -> (usize), blocking
" .callback Name (i64, ptr) -> (void), async
syn region  mlwsvmSignature matchgroup=mlwsvmDirective start=/\%(^\|[^0-9A-Za-z_.$\\]\)\@<=\.\%(func\|extern\|callback\)\>/ end=/$/ keepend oneline contains=mlwsvmFuncName,mlwsvmSigModifier,mlwsvmSigType,mlwsvmArrow,mlwsvmString,mlwsvmMacroArg,mlwsvmComment
syn match   mlwsvmFuncName  contained /\%(\.\%(func\|extern\|callback\)\s\+\)\@<=\%([A-Za-z_.$]\|\\\%(\h\w*\|@\|()\)\)\%([0-9A-Za-z_.$]\|\\\%(\h\w*\|@\|()\)\)*/
" A parameter or a result: [out] TYPE [name], after (, a comma or ->
syn match   mlwsvmSigModifier contained /\%(\%([(,]\|->\)\s*\)\@<=out\ze\s/ nextgroup=mlwsvmSigType skipwhite
syn match   mlwsvmSigType   contained /\%(\%([(,]\|->\)\s*\%(out\s\+\)\=\)\@<=\%(b\|c\|i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\|f32\|f64\|r\|ref\|p\|ptr\|big\|dec\|cstr\|buf\|usize\|isize\|void\)\%(<[0-9A-Za-z_$]\+>\)\=\%([0-9A-Za-z_$]\)\@!/ nextgroup=mlwsvmParam skipwhite
syn match   mlwsvmSigType   contained /\%(\%([(,]\|->\)\s*\%(out\s\+\)\=\)\@<=\%(struct\*\=\|callback\)\ze\s/ nextgroup=mlwsvmTypeName skipwhite
syn match   mlwsvmParam     contained /[A-Za-z_$][0-9A-Za-z_$]*/
syn match   mlwsvmTypeName  contained /[A-Za-z_$][0-9A-Za-z_$]*/

" .macro NAME a, b=default, rest...
syn region  mlwsvmMacroDef  matchgroup=mlwsvmDirective start=/^\s*\.macro\>/ end=/$/ keepend oneline contains=mlwsvmMacroParam,mlwsvmMacroName,mlwsvmComment,mlwsvmString,mlwsvmNumber,mlwsvmRegister,mlwsvmMacroArg
syn match   mlwsvmMacroParam contained /\%([[:space:],]\)\@<=[A-Za-z_$][0-9A-Za-z_$]*/
syn match   mlwsvmMacroName contained /\%(\.macro\s\+\)\@<=[A-Za-z_.$][0-9A-Za-z_.$]*/

" ---- structs ----------------------------------------------------------------

" .struct Name / .cstruct Name, field: type lines, .ends
syn region  mlwsvmStruct    matchgroup=mlwsvmDirective start=/^\s*\.c\=struct\>/ end=/^\s*\.ends\>/ contains=mlwsvmStructName,mlwsvmMember,@mlwsvmOperands fold
syn match   mlwsvmStructName contained /\%(\.c\=struct\s\+\)\@<=[A-Za-z_$][0-9A-Za-z_.$]*/
syn match   mlwsvmMember    contained /^\s*\zs[A-Za-z_$][0-9A-Za-z_$]*\ze\s*:/ nextgroup=mlwsvmMemberColon skipwhite
syn match   mlwsvmMemberColon contained /:/ nextgroup=mlwsvmMemberType skipwhite
syn match   mlwsvmMemberType contained /[A-Za-z_$][0-9A-Za-z_<>$*]*/ nextgroup=mlwsvmTypeName skipwhite

" ---- highlighting -------------------------------------------------------------

hi def link mlwsvmComment     Comment
hi def link mlwsvmTodo        Todo
hi def link mlwsvmTestKey     SpecialComment
hi def link mlwsvmString      String
hi def link mlwsvmEscape      SpecialChar
hi def link mlwsvmChar        Character
hi def link mlwsvmNumber      Number
hi def link mlwsvmConstant    Constant
hi def link mlwsvmLabelRef    Label
hi def link mlwsvmRegister    Identifier
hi def link mlwsvmMacroArg    Special
hi def link mlwsvmArrow       Operator
hi def link mlwsvmStructRef   Type
hi def link mlwsvmField       Identifier
hi def link mlwsvmDirective   PreProc
hi def link mlwsvmInsn        Statement
hi def link mlwsvmInsnAfter   Statement
hi def link mlwsvmSuffix      Type
hi def link mlwsvmLabel       Label
hi def link mlwsvmLabelNext   Label
hi def link mlwsvmFuncName    Function
hi def link mlwsvmSigModifier StorageClass
hi def link mlwsvmSigType     Type
hi def link mlwsvmParam       Identifier
hi def link mlwsvmTypeName    Type
hi def link mlwsvmMacroName   Function
hi def link mlwsvmMacroParam  Identifier
hi def link mlwsvmStructName  Type
hi def link mlwsvmMember      Identifier
hi def link mlwsvmMemberType  Type

let b:current_syntax = 'mlwsvm'

let &cpo = s:cpo_save
unlet s:cpo_save
