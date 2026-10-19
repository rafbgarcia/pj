#!/usr/bin/env python3
"""Copy results/<machine>.json into ../../site/data and write the machine index the site reads."""
import json, os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, '../../site/data')
ORDER = {  # id -> label (display order)
    'c8i': 'Intel Xeon 6975P-C, Granite Rapids (c8i.4xlarge)',
    'c8a': 'AMD EPYC 9R45, Zen 5 (c8a.4xlarge)',
    'c8g': 'AWS Graviton4, Neoverse V2 (c8g.4xlarge)',
}

def main():
    os.makedirs(SITE, exist_ok=True)
    idx = []
    for mid, label in ORDER.items():
        src = os.path.join(HERE, 'results', mid + '.json')
        if os.path.exists(src):
            shutil.copy(src, os.path.join(SITE, mid + '.json'))
            idx.append({'id': mid, 'label': label})
    json.dump(idx, open(os.path.join(SITE, 'index.json'), 'w'), indent=1)
    print('published', [m['id'] for m in idx])

if __name__ == '__main__':
    main()
