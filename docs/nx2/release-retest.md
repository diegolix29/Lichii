# Ask reporters to retest a release

To opt issues into the automatic retest workflow, add one line to the release
notes before publishing a stable release:

```html
<!-- lichii-retest: #123, #456 -->
```

List only reports the release is expected to address and that can be closed if
no one reports the problem again. Ordinary issue links, pull request links,
and game names do not select reports. With no list, the workflow changes
nothing. Closed, locked, assigned, milestoned, and exempt-labeled issues
(including `investigating`) remain excluded.

To preview or resume a retest after publishing, dispatch **Mark issues obsolete
on release** from `nx2`. Set the release tag and enable `dry_run` for a preview.
To supply targets for a release without a list, or to override its list, enter
comma-separated issue numbers in `issues`. A rerun of the original Actions job
uses the workflow at its original revision.

The workflow logs failed issue numbers and processes the other targets before
reporting failure. Retry by dispatching the same release and targets again. A
saved notice for that release is reused, and an issue that has already been
marked for the release will not be marked again. The workflow does not remove
labels from issues marked by older versions of the workflow.
