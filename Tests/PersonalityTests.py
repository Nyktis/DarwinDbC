# SPDX-License-Identifier: BSD-2-Clause
import plistlib
from pathlib import Path


def check_personalities():
    path = Path(__file__).resolve().parents[1] / 'Sources/Darwin/Info.plist'
    configuration = plistlib.loads(path.read_bytes())
    personalities = configuration['IOKitPersonalities']
    assert set(personalities) == {'DarwinDbC'}
    bootstrap = personalities['DarwinDbC']
    assert bootstrap['IOClass'] == 'DarwinDbC'
    assert bootstrap['IOMatchCategory'] == 'DarwinDbC'
    assert bootstrap['CFBundleIdentifier'] == configuration['CFBundleIdentifier']
    assert bootstrap['IOProviderClass'] == 'IOResources'
    assert bootstrap['IOResourceMatch'] == 'IOKit'
    print('IOKit personality checks passed')


if __name__ == '__main__':
    check_personalities()
