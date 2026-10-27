"""End-to-end native backend tests using only generated presets and local temp files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
import time
import zipfile


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--backend',type=Path,required=True);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();binary=str(args.backend.resolve())
    checks=[]
    def run(*words,ok=True):
        p=subprocess.run([binary,*map(str,words)],capture_output=True,text=True,timeout=20)
        if ok:assert p.returncode==0,(words,p.stdout,p.stderr)
        return p
    with tempfile.TemporaryDirectory(prefix='cab-cpp-音-') as directory:
        base=Path(directory);library=base/'Future Plugin/Presets';factory=library/'Factory/Tone.xml';user=library/'User/Own.xml'
        original=b'<future name="untouched" cabActive="true"><mic active="0"/></future>'
        for p in (factory,user):p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(original)
        # Direct edit and a verified, independently readable ZIP; retain inode/owner.
        before=factory.stat();backup=base/'backup.zip'
        run('edit','disable',factory,'--apply','--backup',backup)
        assert factory.read_bytes()==original.replace(b'cabActive="true"',b'cabActive="false"')
        after=factory.stat();assert (before.st_dev,before.st_ino,before.st_uid,before.st_gid)==(after.st_dev,after.st_ino,after.st_uid,after.st_gid)
        with zipfile.ZipFile(backup) as z:
            assert z.testzip() is None
            manifest=json.loads(z.read('backup-manifest.json'));record=manifest['files'][0]
            assert record['entry']=='presets/Tone.xml' and z.namelist()==['presets/Tone.xml','backup-manifest.json']
            assert z.read(record['entry'])==original and record['sha256']==hashlib.sha256(original).hexdigest()
        checks.append('verified_zip_before_direct_write_and_inode_owner_retention')
        # Any backup failure blocks writes, including a pre-existing destination.
        snapshot=user.read_bytes();p=run('edit','disable',user,'--apply','--backup',backup,ok=False)
        assert p.returncode and user.read_bytes()==snapshot
        blocked=base/'not-a-directory';blocked.write_bytes(b'block')
        assert run('edit','disable',user,'--apply','--backup',blocked/'x.zip',ok=False).returncode
        assert user.read_bytes()==snapshot;checks.append('failed_backup_no_writes')
        # Copies preserve embedded metadata, include User, preserve hierarchy, no overwrite.
        run('edit','copy',user,'--library',library,'--apply')
        copy=library/'Cab Disabled/User/Own.xml';assert copy.read_bytes()==original.replace(b'true',b'false') and user.read_bytes()==original
        run('edit','copy-enable',user,'--library',library,'--apply')
        enabled_copy=library/'Cab Enabled/User/Own.xml'
        assert enabled_copy.read_bytes()==original and user.read_bytes()==original
        assert copy.read_bytes()==original.replace(b'true',b'false')
        assert 'already-correct' in run('edit','copy-enable',user,'--library',library,'--apply').stdout
        assert 'already-correct' in run('edit','copy',user,'--library',library,'--apply').stdout
        copy.write_bytes(b'collision');assert run('edit','copy',user,'--library',library,'--apply',ok=False).returncode and copy.read_bytes()==b'collision'
        prior=base/'Already CD.XML';prior.write_bytes(original)
        run('edit','copy',prior,'--library',base,'--apply')
        assert (base/'Cab Disabled/Already CD.XML').read_bytes()==original.replace(b'true',b'false')
        assert not (base/'Cab Disabled/Already CD CD.XML').exists()
        checks.append('copy_user_hierarchy_filename_metadata_and_no_clobber')
        if os.name!='nt':
            alias=base/'alias.xml';os.link(user,alias)
            assert '"changed":1' in run('edit','disable',user,alias,'--apply').stdout
            link=base/'symlink.xml';link.symlink_to(factory)
            assert run('inspect',link,ok=False).returncode
            checks.append('hardlink_dedup_and_symlink_rejection')
        # Drive the real pipe service, with async worker snapshots and mutations.
        process=subprocess.Popen([binary],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
        lines=queue.Queue()
        def read(stream=process.stdout,target=lines):
            for line in stream:target.put(line.rstrip('\r\n'))
        threading.Thread(target=read,daemon=True).start()
        indexing_snapshots=[]
        def ready(page):
            end=time.monotonic()+15;state=None;controls={};current=None
            while time.monotonic()<end:
                line=lines.get(timeout=15).split('\t')
                if line[0]=='ERROR':raise AssertionError(bytes.fromhex(line[1]).decode())
                if line[0]=='STATE':state=line;controls={}
                if line[0]=='CONTROL':current=line[1];controls[current]={'kind':line[2],'label':bytes.fromhex(line[4]).decode(),'value':bytes.fromhex(line[5]).decode(),'options':[]}
                if line[0]=='OPTION':controls[current]['options'].append(line)
                if line[0]=='END' and state and int(state[1])==0 and state[2]=='1':
                    assert not controls['plugins']['options'], 'Indexing exposed stale/partial plugins'
                    assert bytes.fromhex(state[7]).decode()=='', 'Indexing leaked into footer status'
                    assert state[4]=='0', 'Next enabled during indexing'
                    indexing_snapshots.append(True)
                if line[0]=='END' and state and int(state[1])==page and state[2]=='0':return controls
            raise AssertionError('No ready snapshot')
        def send(action,**fields):
            process.stdin.write(action+''.join('\t'+k+'='+str(v).encode().hex() for k,v in fields.items())+'\n');process.stdin.flush()
        ready(0);send('add',path=str(library.parent));view=ready(0);assert len(view['plugins']['options'])==1
        assert len(indexing_snapshots)==2, 'Missing startup/Browse indexing state'
        assert view['browse']['label']=='Preset folders not detected?'
        send('scan');view=ready(0)
        assert len(indexing_snapshots)==3 and len(view['plugins']['options'])==1
        send('next',plugins='0');view=ready(1)
        leaves=[o for o in view['presets']['options'] if o[6]=='preset']
        assert any(bytes.fromhex(o[3]).decode()=='Own' and o[4]=='0' for o in leaves)
        # Both generated copy directories must stay out of the selection tree.
        assert len(leaves)==2 and {bytes.fromhex(o[3]).decode() for o in leaves}=={'Tone','Own'}
        own=next(o[2] for o in leaves if bytes.fromhex(o[3]).decode()=='Own');own=bytes.fromhex(own).decode()
        user.write_bytes(original)
        send('next',presets=own);backup_view=ready(2)
        assert re.fullmatch(r'neural_dsp_presets_\d{4}_\d{2}_\d{2}(?:_\d+)?\.zip',Path(backup_view['backup_path']['value']).name)
        second_backup=base/'selected.zip';send('next',backup='1',backup_path=str(second_backup));edit_view=ready(3)
        for key,labels in [('disable',['Disable','Enable']),('copy',['Direct Edit','Copy and Edit'])]:
            options=edit_view[key]['options']
            assert [bytes.fromhex(o[3]).decode() for o in options]==labels
            assert [o[4] for o in options]==['1','0'], 'Left option must be selected by default'
        direct_text=bytes.fromhex(edit_view['copy']['options'][0][8]).decode()
        assert direct_text.splitlines()==[
            'Modify the saved presets directly, maintaining their names and locations within the preset browser.',
            'Direct edit may require elevated permissions.']
        assert len(bytes.fromhex(edit_view['copy']['options'][1][8]).decode().splitlines())==1
        send('next',disable='1',copy='0');view=ready(4)
        assert not second_backup.exists() and 'Operation: Disable' in view['review_summary']['value'] and 'Presets: 1' in view['review_summary']['value'] and 'Own.xml' not in view['report']['value']
        send('back');ready(3);send('next',disable='0',copy='0');enabled_view=ready(4)
        assert enabled_view['review_summary']['value'].startswith('Mode: Direct edit\nBackup: '+str(second_backup)+'\nOperation: Enable\nPresets: 1')
        send('back');ready(3);send('next',disable='1',copy='0');ready(4)
        send('next');view=ready(5);assert view['summary']['value']=='Successful: 1\nFailed: 0' and second_backup.exists()
        with zipfile.ZipFile(second_backup) as z:assert len(json.loads(z.read('backup-manifest.json'))['files'])==1
        assert user.read_bytes()==original.replace(b'true',b'false')
        send('quit');process.stdin.close();assert process.wait(timeout=10)==0
        checks.append('async_protocol_browse_user_selection_review_backup_apply_results_shutdown')
        # A missing reviewed source stops the whole direct batch before any write.
        # Repeated batch errors must retain relative labels without exposing paths.
        for path in (factory,user):path.write_bytes(original)
        prefix_sibling=factory.with_name("Tone.xml.xml");prefix_sibling.write_bytes(original)
        process=subprocess.Popen([binary],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
        lines=queue.Queue();threading.Thread(target=read,args=(process.stdout,lines),daemon=True).start()
        ready(0);send('add',path=str(library.parent));ready(0);send('next',plugins='0');view=ready(1)
        selected=','.join(bytes.fromhex(o[2]).decode() for o in view['presets']['options'] if o[6]=='preset')
        send('next',presets=selected);ready(2);send('next',backup='0');ready(3);send('next',disable='1',copy='0');review=ready(4)
        assert review['backup_notice']['value']=='Backup not enabled. Proceed at your own risk.'
        assert list(review)[:2]==['review_summary','report']
        assert review['review_summary']['kind']=='readonly' and review['report']['kind']=='text'
        assert 'Mode:' not in review['report']['value'] and 'Backup:' not in review['report']['value']
        assert review['review_summary']['value'].startswith('Mode: Direct edit\nBackup: No\n')
        prefix_sibling.unlink();send('next');view=ready(5)
        assert view['summary']['value']=='Successful: 0\nFailed: 3'
        assert str(base) not in view['details']['value'] and 'Tone.xml.xml' not in view['details']['value']
        assert factory.read_bytes()==original and user.read_bytes()==original
        send('quit');process.stdin.close();assert process.wait(timeout=10)==0
        checks.append('missing_reviewed_file_stops_batch_and_repeated_errors_use_relative_labels')

    report=dict(checks=checks,passed=True,scope='native C++ backend; generated temporary inputs only')
    args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))

if __name__=='__main__':main()
