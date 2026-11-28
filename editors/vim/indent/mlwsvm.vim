" Vim indent file
" Language:    MLWSVM assembly (.masm)
" Maintainer:  https://github.com/msharpe248/mlws-vm (editors/vim)
" License:     MIT
"
" The layout of the repository's own files: .func, .struct and .macro at the
" left, their bodies one shiftwidth in; labels, .try and conditional and
" repeated assembly at the block's own column.

if exists('b:did_indent')
  finish
endif
let b:did_indent = 1

setlocal indentexpr=MlwsvmIndent(v:lnum)
setlocal indentkeys=!^F,o,O,<:>,0=.endf,0=.ends,0=.endm,0=.try,0=.endtry,0=.if,0=.el,0=.endif,0=.rept,0=.irp,0=.endr
setlocal autoindent

let b:undo_indent = 'setlocal indentexpr< indentkeys< autoindent<'

if exists('*MlwsvmIndent')
  finish
endif

let s:start = '^\s*\.\%(func\|c\=struct\|macro\)\>'
let s:end = '^\s*\.\%(endf\|ends\|endm\)\>'
" At the block's own column: a label (not a struct's field: type), a .try
" block (not .try start, end, handler) and the preprocessor's blocks.
let s:outdent = '^\s*\%(\%([A-Za-z_.$0-9]\|\\@\|\\()\|\\\h\w*\)\+:\%(:\)\@!'
      \ . '\|\.try\>[^,]*$'
      \ . '\|\.\%(endtry\|if\|ifdef\|ifndef\|ifb\|ifnb\|elif\|else\|endif\|rept\|irp\|endr\)\>\)'

function MlwsvmIndent(lnum) abort
  let l:line = getline(a:lnum)
  " The block this line is in: its start line, or 0 at the top level.
  let l:save = getpos('.')
  call cursor(a:lnum, 1)
  if l:line =~# s:end
    let l:open = searchpair(s:start, '', s:end, 'bnW')
    call setpos('.', l:save)
    return l:open > 0 ? indent(l:open) : 0
  endif
  let l:open = searchpair(s:start, '', s:end, 'bnW')
  call setpos('.', l:save)
  if l:open == 0
    return l:line =~# s:start ? 0 : indent(prevnonblank(a:lnum - 1))
  endif
  let l:base = indent(l:open)
  " A struct's fields (x: i64) are its body; elsewhere name: is a label.
  let l:in_struct = getline(l:open) =~# '^\s*\.c\=struct\>'
  if l:line =~# s:start
    return l:base + shiftwidth()
  endif
  if !l:in_struct && l:line =~# s:outdent
    return l:base
  endif
  return l:base + shiftwidth()
endfunction
