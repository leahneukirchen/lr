" :Lr <lr-args> to browse lr(1) results in a new window,
"               press return to open file in new window.
command! -n=* -complete=file Lr silent exe "R" "lr" <q-args> | res | silent f [lr] | map <buffer><CR> $hgF


