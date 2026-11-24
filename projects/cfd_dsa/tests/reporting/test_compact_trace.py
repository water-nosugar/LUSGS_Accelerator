import _support
import csv
from pathlib import Path
import tempfile
import unittest

from trace_text import compact_trace


class CompactTraceTests(unittest.TestCase):
    def test_columns_values_order_and_unknown_events_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'trace.csv'
            with path.open('w',newline='') as output:
                writer=csv.writer(output)
                writer.writerow(['cycle','stage','requestId','generation','note','future_column'])
                writer.writerows([[1,'REQUEST_ALLOC',22,12345,'中文,带逗号',''],
                    [2,'SKIP',22,12345,'x','y'],[3,'FUTURE_STAGE',22,12345,'多行\n内容','z'],
                    [4,'REQUEST_FAILED',22,12345,'error','new']])
            with path.open(newline='') as source:
                expected=[r for r in csv.DictReader(source) if r['stage']!='SKIP']
            rows=compact_trace(path,{'SKIP'})
            self.assertEqual([dict(r) for r in rows],expected)
            self.assertEqual(list(rows[0].values()),list(expected[0].values()))
            self.assertIs(rows[0].indices,rows[1].indices)
            self.assertIs(rows[0]['generation'],rows[1]['generation'])
            self.assertEqual(rows[0].get('absent','default'),'default')
            with self.assertRaises(TypeError):rows[0]['cycle']='99'

    def test_malformed_rows_and_duplicate_header_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'trace.csv'
            for text in ['stage,cycle\nREQUEST_ALLOC\n','stage,cycle\nREQUEST_ALLOC,1,extra\n',
                         'stage,stage\nREQUEST_ALLOC,1\n']:
                path.write_text(text)
                with self.assertRaises(ValueError):compact_trace(path)

    def test_unknown_schema_columns_are_not_silently_removed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'trace.csv';path.write_text('stage,surprise\nUNKNOWN,needed\n')
            self.assertEqual(dict(compact_trace(path)[0]),{'stage':'UNKNOWN','surprise':'needed'})


if __name__=='__main__':unittest.main()
