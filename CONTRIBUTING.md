# Contributing

Contributions are welcome: issues, fixes and features alike.  Before
sending a change, build it on the platforms it affects ([README.md](README.md)).

## The contributor agreement

The GUI is under the BSD 3-Clause License ([LICENSE](LICENSE)).  So that
the D-Modem Authors can offer it under that and other terms, each
contribution comes with this agreement.
By submitting a contribution (a pull request, a patch or
any other code or text meant for inclusion), you agree that:

1. The contribution is your own original work, or you otherwise have
   the right to submit it under these terms, and to your knowledge it
   infringes no one's rights.  If your employer has rights in what you
   create, you have its permission to contribute, or it has waived those
   rights.
2. You keep the copyright in your contribution.
3. You grant the D-Modem Authors, and those who receive the project from them, a
   perpetual, worldwide, non-exclusive, no-charge, royalty-free,
   irrevocable license to use, copy, modify, publicly display and
   perform, sublicense and distribute your contribution and works based
   on it, under the BSD 3-Clause License and under any other terms, commercial
   licenses included.
4. You grant the same parties a license, on the same terms, under any
   patent claims of yours that your contribution would infringe, to
   make, use, sell and otherwise transfer it as part of the project.
5. Your contribution is provided "as is", without warranties or
   conditions of any kind.

Please add this line to each commit you contribute (`git commit -s`
does it):

    Signed-off-by: Your Name <your@email>

It says that you agree to the above for that commit.  A contribution
without it cannot be merged.

A check on each pull request looks for that line, with the commit
author's email, on every commit.  If it's missing, `git rebase --signoff
main` (or `git commit --amend -s` for the last commit) adds it; then
force-push.  Pull requests from the D-Modem Authors themselves (the
repository's owner and the D-Modem organization's members) are not
checked.
