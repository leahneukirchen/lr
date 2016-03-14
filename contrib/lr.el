;;; lr.el --- run `lr' and display the results

(define-compilation-mode lr-mode "Lr"
  (set (make-local-variable 'compilation-error-regexp-alist)
       '(("\\([^ \n']+\\)[*/=>@|]$" 1)
         ("\\([^ \n']+\\)$" 1)
         ("'\\(.+\\)'$" 1)))
  (set (make-local-variable 'next-error-highlight) nil))

(defun lr (command-args)
  "Run lr, with user-specified args, and collect output in a buffer.
While lr runs asynchronously, you can use \\[next-error] (M-x next-error),
or RET in the *lr* buffer, to go to the files lr found."
  (interactive (list (read-shell-command "Run lr (like this): "
                                         "lr " 'lr-history)))
  (compilation-start command-args 'lr-mode))

(provide 'lr)
;;; lr.el ends here
