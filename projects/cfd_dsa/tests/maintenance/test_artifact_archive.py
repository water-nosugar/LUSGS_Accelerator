"""Cold storage must be reversible and reject corrupted or unsafe evidence."""
import _support
import io
import json
from pathlib import Path
import tempfile
import unittest
import tarfile
from artifact_archive import pack,restore,verify,inventory,file_sha


class ArtifactArchiveTests(unittest.TestCase):
    def fixture(self,root):
        source=root/'case';source.mkdir();(source/'nested').mkdir()
        (source/'nested/input').write_bytes(b'CFD-data\0'*300)
        (source/'reference').symlink_to('nested/input')
        return source

    def test_roundtrip_preserves_all_data_modes_times_and_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=self.fixture(root);before=inventory(source)
            pack(source,root/'store');self.assertFalse(source.exists())
            restore(root/'store/case.index.json')
            self.assertEqual(inventory(source),before)

    def test_corrupt_archive_never_creates_restore_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=self.fixture(root);pack(source,root/'store')
            (root/'store/case.tar.gz').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError,'archive SHA'):restore(root/'store/case.index.json')
            self.assertFalse(source.exists())

    def test_refuses_to_overwrite_existing_archive_or_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=self.fixture(root);pack(source,root/'store')
            source.mkdir();(source/'keep').write_text('retain')
            with self.assertRaises(FileExistsError):restore(root/'store/case.index.json')
            with self.assertRaises(FileExistsError):pack(source,root/'store')
            self.assertEqual((source/'keep').read_text(),'retain')

    def test_rejects_archive_path_traversal_even_with_matching_container_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);archive=root/'bad.tar.gz'
            with tarfile.open(archive,'w:gz') as tar:
                item=tarfile.TarInfo('case/../escape');item.size=1;tar.addfile(item,io.BytesIO(b'x'))
            index=root/'case.index.json';index.write_text(json.dumps(dict(name='case',archive=archive.name,archive_sha256=file_sha(archive),original_path=str(root/'case'),tree={})))
            with self.assertRaisesRegex(ValueError,'unsafe archive'):restore(index)
            self.assertFalse((root/'escape').exists())


class CleanupProvenanceTests(unittest.TestCase):
    def test_archived_name_is_reserved_before_any_output_or_process_creation(self):
        from contextlib import redirect_stderr
        from unittest.mock import patch
        import run_experiment
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);index=root/'archive/campaigns-example/old.index.json'
            index.parent.mkdir(parents=True);index.write_text('{}')
            with patch.object(run_experiment,'RESULT_ROOT',root),patch('sys.argv',[
                    'runner','--target','lusgs-single-line','--campaign','old','--case','new']),redirect_stderr(io.StringIO()) as errors:
                with self.assertRaises(SystemExit) as stopped:run_experiment.main()
            self.assertEqual(stopped.exception.code,2)
            self.assertIn('campaign is archived',errors.getvalue())
            self.assertFalse((root/'campaigns').exists())

    def test_nested_tests_fixtures_and_prototypes_remain_in_source_provenance(self):
        from unittest.mock import patch
        import run_experiment
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            names=['projects/cfd_dsa/tests/arithmetic/test_example.py',
                   'projects/cfd_dsa/tests/fixtures/example.cc',
                   'projects/cfd_dsa/experiments/affine_example.py']
            for name in names:
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('original')
            with patch.object(run_experiment,'REPO',root):
                original=run_experiment.source_fingerprint()
                self.assertEqual(set(original),set(names))
                (root/names[0]).write_text('modified')
                self.assertNotEqual(original,run_experiment.source_fingerprint())
