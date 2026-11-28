" MLWSVM assembly: .masm files, and .inc files that sit with them.
"
" Vim gives *.masm to Microsoft's assembler (filetype masm), so this uses
" :set filetype rather than :setfiletype, to override it.

autocmd BufNewFile,BufRead *.masm set filetype=mlwsvm

" An included file (.inc) is MLWSVM assembly when .masm files sit in its
" directory or the one above it (lib/std, tests/asm/_inc).
function! s:DetectInc() abort
  let l:dir = expand('<afile>:p:h')
  if !empty(glob(l:dir . '/*.masm', 1, 1)) || !empty(glob(l:dir . '/../*.masm', 1, 1))
    set filetype=mlwsvm
  endif
endfunction
autocmd BufNewFile,BufRead *.inc call s:DetectInc()
