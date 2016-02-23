" :Lr <lr-args> to browse lr(1) results in a new window,
"               press return to open file in new window.
command! -nargs=* -complete=file Lr
	\ new | setl bt=nofile noswf | silent exe "0r!lr -Q " <q-args> |
	\ 0 | res | map <buffer><C-M> $<C-W>F<C-W>_
