"""Read-only upstream Expat check. Run before a public build; no runtime networking."""
import argparse
import json
from pathlib import Path
import re
import urllib.request

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    header=(ROOT/'third_party/expat/expat.h').read_text()
    version='.'.join(re.search(r'#\s*define XML_'+part+r'_VERSION\s+(\d+)',header).group(1) for part in ('MAJOR','MINOR','MICRO'))
    def get(path):
        request=urllib.request.Request('https://api.github.com/repos/libexpat/libexpat/'+path,headers={'User-Agent':'Archetype-Cab-Disabler-build-review'})
        with urllib.request.urlopen(request,timeout=30) as response:return json.load(response)
    release=get('releases/latest');issues=get('issues/1160')
    unresolved=[line[6:] for line in issues['body'].splitlines() if line.startswith('- [ ] ')]
    report=dict(vendored_version=version,latest_release=release['tag_name'],release_url=release['html_url'],
                matches_latest=release['tag_name']=='R_'+version.replace('.','_'),
                unresolved_upstream_reports=unresolved,applicability='unknown; public details do not establish exposure',
                issue_url=issues['html_url'],issue_updated=issues['updated_at'])
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"Expat {version}; latest {release['tag_name']}; {len(unresolved)} unresolved upstream reports (applicability unknown)")
    if not report['matches_latest']:raise SystemExit('Review newer upstream release before publishing')


if __name__=='__main__':main()
