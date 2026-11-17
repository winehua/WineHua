"""Fetch a pinned public release asset with bounded metadata fallback."""
import json
import urllib.error
import urllib.parse
import urllib.request

def open_distribution(dist, repository, opener=None):
    """Use GitHub's asset API if its browser-download gateway is unavailable."""
    opener = opener or urllib.request.urlopen
    prefix = 'https://github.com/' + repository + '/releases/download/'
    if not dist['source'].startswith(prefix):
        raise ValueError('Unexpected distribution URL')
    headers = {'User-Agent': 'WineHua-build/1'}
    try:
        return opener(urllib.request.Request(dist['source'], headers=headers), timeout=90)
    except urllib.error.HTTPError as error:
        if error.code not in (429, 500, 502, 503, 504):
            raise
        error.close()
    tag, filename = dist['source'].removeprefix(prefix).split('/')
    if filename != dist['file']:
        raise ValueError('source filename differs from distribution')
    api = 'https://api.github.com/repos/' + repository + '/releases/'
    request = urllib.request.Request(api + 'tags/' + urllib.parse.quote(tag, safe=''),
                                     headers={**headers, 'Accept': 'application/vnd.github+json'})
    with opener(request, timeout=90) as response:
        metadata = response.read(1024 * 1024 + 1)
    if len(metadata) > 1024 * 1024:
        raise ValueError('Oversized release metadata')
    release = json.loads(metadata)
    assets = [asset for asset in release.get('assets', [])
              if asset.get('name') == filename and asset.get('size') == dist['bytes'] and
              asset.get('browser_download_url') == dist['source']]
    if release.get('tag_name') != tag or len(assets) != 1:
        raise ValueError('GitHub asset differs from pinned distribution')
    asset_url = assets[0].get('url', '')
    if not asset_url.startswith(api + 'assets/') or not asset_url.removeprefix(api + 'assets/').isdigit():
        raise ValueError('Unexpected GitHub asset API URL')
    return opener(urllib.request.Request(asset_url,
        headers={**headers, 'Accept': 'application/octet-stream'}), timeout=90)
