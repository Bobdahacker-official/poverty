const { execFileSync } = require('node:child_process');

function git(...args) {
  return execFileSync('git', args, { maxBuffer: 64 * 1024 * 1024 });
}

const artifacts = [
  ['index.html', 'drawing-inc.html'],
  ['avalanche/images/avalanche.elf', 'avalanche.elf'],
  ['avalanche/images/avalanche.iso', 'avalanche.iso'],
  ['avalanche/images/avalanche-data.img', 'avalanche-data.img'],
  ['avalanche/images/NOTICE', 'avalanche-NOTICE.txt'],
  ['avalanche/images/GRUB-COPYRIGHT', 'GRUB-COPYRIGHT.txt'],
  ['avalanche/assets/FONT-LICENSE', 'FONT-LICENSE.txt'],
  ['magenta/images/magenta.iso', 'magenta.iso'],
  ['magenta/images/magenta-data.img', 'magenta-data.img'],
  ['magenta/images/NOTICE', 'magenta-NOTICE.txt'],
  ['magenta/images/desktop.png', 'magenta-desktop.png'],
];

module.exports = async function publish({ github, context, core }) {
  const { owner, repo } = context.repo;
  const commits = git('rev-list', '--reverse', 'HEAD').toString().trim().split('\n').filter(Boolean);
  const existing = await github.paginate(github.rest.repos.listReleases, { owner, repo, per_page: 100 });
  const byTag = new Map(existing.map(release => [release.tag_name, release]));
  const latest = commits.at(-1);

  for (const sha of commits) {
    const short = sha.slice(0, 12);
    const tag = `commit-${sha}`;
    const tags = git('tag', '--points-at', sha).toString().trim().split('\n').filter(Boolean);
    const other = tags.map(name => byTag.get(name)).find(release => release && !release.draft && release.tag_name !== tag);
    if (other) {
      core.info(`${short} already has release ${other.tag_name}; preserving it`);
      continue;
    }

    const subject = git('show', '-s', '--format=%s', sha).toString().trim();
    const message = git('show', '-s', '--format=%B', sha).toString().trim();
    const date = git('show', '-s', '--format=%cI', sha).toString().trim();
    let release = byTag.get(tag);
    if (!release) {
      const response = await github.rest.repos.createRelease({
        owner, repo,
        tag_name: tag,
        target_commitish: sha,
        name: `${short} — ${subject}`.slice(0, 250),
        body: `Commit: [${sha}](https://github.com/${owner}/${repo}/commit/${sha})\n\nCommitted: ${date}\n\n${message}`,
        draft: false,
        prerelease: false,
        make_latest: sha === latest ? 'true' : 'false',
      });
      release = response.data;
      byTag.set(tag, release);
      core.info(`Published ${release.html_url}`);
    }

    const present = new Set(git('ls-tree', '-r', '--name-only', sha).toString().split('\n'));
    const uploaded = await github.paginate(github.rest.repos.listReleaseAssets, {
      owner, repo, release_id: release.id, per_page: 100,
    });
    const names = new Set(uploaded.map(asset => asset.name));
    for (const [path, name] of artifacts) {
      if (!present.has(path) || names.has(name)) continue;
      const data = git('show', `${sha}:${path}`);
      await github.rest.repos.uploadReleaseAsset({
        owner, repo, release_id: release.id, name, data,
        headers: { 'content-type': 'application/octet-stream', 'content-length': data.length },
      });
      core.info(`Attached ${name} to ${short}`);
    }

    if (sha === latest) {
      await github.rest.repos.updateRelease({ owner, repo, release_id: release.id, make_latest: 'true' });
    }
  }

  await core.summary
    .addHeading('Commit releases')
    .addRaw(`Processed ${commits.length} commits through ${latest}.`)
    .addLink('Browse releases', `https://github.com/${owner}/${repo}/releases`)
    .write();
};
