# Maintaining and publishing the SORX wiki

The canonical Markdown is in `doc/wiki`: 16 topic pages plus `_Sidebar.md` and `_Footer.md`. Publish those files unchanged to the separate [SORX wiki](https://github.com/lehajamb/sorx/wiki) Git repository. This maintenance guide stays in the source repository.

The initial wiki documents source `3234de923d6497b9a119c80485a717ac24d4e6fe`. It is delivered on `codex/wiki-docs` with a documentation-only PR left open for review. Its documentation-source links intentionally use that branch until merge; update them to `main` before retiring the branch. Code links remain pinned to the documented source revision.

## Author and validate

Work from the selected published source revision and confirm a clean working tree before creating a documentation branch. Derive contracts from headers, implementation and tests, using design and qualification documents with their stated dates and status. Keep embedded INI/runtime/journal contracts distinct from appliance DeviceConfig, bounded dispatch and hardware gates.

Preserve page filenames because they determine URLs. Use absolute wiki URLs for page links, and update Home and `_Sidebar.md` when changing navigation. When documenting a later source revision, update the source links, Home revision and footer revision/review date together. Do not copy historical test counts as fresh results.

Before publishing:

- Check every wiki destination and heading anchor, source path at its pinned commit, code-fence pair, table and Mermaid diagram.
- Confirm copied configuration keys/values and CLI arguments against the implementation. Label abbreviated fragments as excerpts.
- Compile new C++ examples against the public headers and their documented library targets.
- Run the Windows walkthrough build, demo and CTest; validate the sample DeviceConfig and schema checker. Record unavailable optional QuickFIX, Linux and hardware checks explicitly.
- Review `git diff --check`, the full documentation diff and any independent changes in the live wiki.

The original Linux CI failure is a source baseline issue, documented in Testing and Troubleshooting. Do not weaken compiler flags or silently repair application behavior as part of a wiki-only update.

## Source review and credentials

Commit the prepared Markdown and README link on a documentation branch, push that branch, and open a documentation-only PR. Initial publication may precede merge, matching the MockX workflow; the PR and live wiki must contain the same reviewed Markdown. Record the wiki commit in the PR after publication.

Use the existing `lehajamb` SSH alias from Git Bash. Its interactive startup unlocks the protected key and shares the agent with later Git Bash windows. Verify the account without printing credentials:

```bash
ssh -T git@github-lehajamb
gh api user --jq .login
```

The SSH greeting must identify `lehajamb`; GitHub normally returns exit code 1 after a successful greeting. If needed, run `github_agent_init` in Git Bash and enter the passphrase only at the local hidden prompt. GitHub CLI authentication is separate from SSH; use the existing login or authorize `gh auth login --hostname github.com --web --git-protocol https --scopes repo,workflow` as `lehajamb`. Do not publish credentials in documentation, commits or chat.

SORX's wiki is already enabled and initialized. If rebuilding this setup for another repository, an initial page must exist before its `.wiki.git` repository can be cloned. Avoid reinitializing an existing wiki.

## Prepare the wiki checkout

Run from the reviewed source checkout in Git Bash. Use a fresh ignored directory; `build-wiki-publish` is covered by `/build-*/`. If that directory already exists, inspect and reuse it deliberately or choose another `build-*` directory instead of deleting its history.

```bash
wiki_checkout="$PWD/build-wiki-publish"
git clone git@github-lehajamb:lehajamb/sorx.wiki.git "$wiki_checkout"
git -C "$wiki_checkout" branch --show-current
git -C "$wiki_checkout" status --short
git -C "$wiki_checkout" config --show-origin --get user.name
git -C "$wiki_checkout" config --show-origin --get user.email
```

Stop if cloning fails, the checkout is dirty, or the account/identity is unexpected. The initialized wiki uses `master`; confirm the actual checked-out default branch rather than renaming it. The SSH alias selects `lehajamb / lehajam+b@gmail.com` on the configured maintainer workstation. Other contributors must use their own authorized identity.

Inspect existing pages before copying. If a same-named page has independent edits, reconcile them into `doc/wiki` first and update the source PR. Preserve unrelated pages. Removing a canonical source file does not automatically authorize deleting its published counterpart.

```bash
cp -- doc/wiki/*.md "$wiki_checkout/"
git -C "$wiki_checkout" diff --check
git -C "$wiki_checkout" diff --stat
git -C "$wiki_checkout" diff
```

Inspect newly copied untracked pages as well; an unstaged diff does not show their contents. Review the complete staged diff in the next step before committing.

## Commit and publish

Stage only the filenames owned by the canonical source:

```bash
wiki_files=()
for wiki_page in doc/wiki/*.md; do
  wiki_files+=("${wiki_page##*/}")
done
git -C "$wiki_checkout" add -- "${wiki_files[@]}"
git -C "$wiki_checkout" diff --cached --check
git -C "$wiki_checkout" diff --cached
```

If there is no staged difference, no publication commit is needed. Otherwise, after checking every command succeeded:

```bash
git -C "$wiki_checkout" commit -m "docs: publish SORX user and developer wiki"
git -C "$wiki_checkout" push origin HEAD
git -C "$wiki_checkout" rev-parse HEAD
```

Push only after a successful commit. If another writer has advanced the wiki, fetch and reconcile the new history and update canonical source before retrying. Never force-push. If publication is blocked, retain the prepared files and report the exact failed step.

## Verify and record

Compare every published Markdown file byte-for-byte against `doc/wiki`. Confirm the remote wiki branch points to the publication commit. Open Home, Build and Installation, Embedded Cpp API, Device Configuration and CLI, Persistence and Recovery, and Appliance Execution Foundation in a browser; check headings, code, tables, diagrams, sidebar and footer. Follow the maintenance link and representative pinned source links.

Record the source PR URL, documented application revision, published wiki commit, actual test results and any skipped checks. Leave the initial source PR unmerged unless its owner requests a merge. This process installs no background synchronization; wiki publication and the existing GitHub Pages workflow are separate.
