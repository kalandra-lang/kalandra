"""Check Microsoft's public SDK catalog before accepting any payload downloads."""
import hashlib
import json
from urllib.request import Request, urlopen

CHANNEL = 'https://aka.ms/vs/18/stable/channel'
CHANNEL_SHA256 = '244745205123668d1184dee407a48b7514713090e9dae5c59da549663d772462'


def fetch(url):
    with urlopen(Request(url, headers={'Accept-Encoding': 'identity'}), timeout=120) as response:
        return response.read()


def check():
    channel = fetch(CHANNEL)
    if hashlib.sha256(channel).hexdigest() != CHANNEL_SHA256:
        raise ValueError('Microsoft channel changed; review and pin its new catalog explicitly')
    catalog, = [entry for entry in json.loads(channel)['channelItems'] if entry.get('type') == 'Manifest']
    payload, = catalog['payloads']
    data = fetch(payload['url'])
    actual = hashlib.sha256(data).hexdigest()
    print(json.dumps(dict(url=payload['url'], expectedSHA256=payload['sha256'],
                          actualSHA256=actual, expectedBytes=payload['size'], actualBytes=len(data))), flush=True)
    if actual != payload['sha256'] or len(data) != payload['size']:
        raise ValueError('Microsoft catalog does not match its published digest/size; no SDK payloads accepted')
    return channel, data


if __name__ == '__main__': check()
