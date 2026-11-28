" Vim filetype plugin
" Language:    MLWSVM assembly (.masm)
" Maintainer:  https://github.com/msharpe248/mlws-vm (editors/vim)
" License:     MIT

if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

let s:cpo_save = &cpo
set cpo&vim

setlocal comments=:;
setlocal commentstring=;\ %s
setlocal formatoptions-=t formatoptions+=croql
" Identifiers hold dots and $ (std.io.print, add.i32), so w and * take them whole.
setlocal iskeyword=@,48-57,_,.,$

" The repository's style: four spaces. let g:mlwsvm_recommended_style = 0
" keeps your own settings.
if get(g:, 'mlwsvm_recommended_style', 1)
  setlocal expandtab shiftwidth=4 softtabstop=4
  let b:mlwsvm_set_style = 1
endif

" % jumps between a block's directives (with matchit).
let b:match_ignorecase = 0
let b:match_words =
      \ '^\s*\.func\>:^\s*\.endf\>,' .
      \ '^\s*\.c\=struct\>:^\s*\.ends\>,' .
      \ '^\s*\.macro\>:^\s*\.endm\>,' .
      \ '^\s*\.try\>:^\s*\.endtry\>,' .
      \ '^\s*\.if\%(n\=def\|n\=b\)\=\>:^\s*\.elif\>:^\s*\.else\>:^\s*\.endif\>,' .
      \ '^\s*\.\%(rept\|irp\)\>:^\s*\.endr\>'

" Folding: :setlocal foldmethod=expr folds .func, .struct, .macro, .try,
" .if and .rept blocks; let g:mlwsvm_folding = 1 turns it on for every file.
setlocal foldexpr=MlwsvmFold(v:lnum)
if get(g:, 'mlwsvm_folding', 0)
  setlocal foldmethod=expr
endif

if !exists('*MlwsvmFold')
  function MlwsvmFold(lnum) abort
    let l:line = getline(a:lnum)
    if l:line =~# '^\s*\.\%(func\|c\=struct\|macro\|try\|if\|ifdef\|ifndef\|ifb\|ifnb\|rept\|irp\)\>'
      return 'a1'
    elseif l:line =~# '^\s*\.\%(endf\|ends\|endm\|endtry\|endif\|endr\)\>'
      return 's1'
    endif
    return '='
  endfunction
endif

let b:undo_ftplugin = 'setlocal comments< commentstring< formatoptions< iskeyword< foldexpr< foldmethod<'
      \ . ' | unlet! b:match_words b:match_ignorecase'
if exists('b:mlwsvm_set_style')
  let b:undo_ftplugin .= ' | setlocal expandtab< shiftwidth< softtabstop< | unlet b:mlwsvm_set_style'
endif

let &cpo = s:cpo_save
unlet s:cpo_save
