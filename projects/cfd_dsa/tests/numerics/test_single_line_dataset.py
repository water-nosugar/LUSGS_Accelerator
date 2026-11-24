import _support
import io
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = _support.PROJECT_ROOT
from extract_gt01r import block_path, graph, market, records_for_path
from single_line_dataset import HEADER, RECORD, fnv64, oracle, read_dataset, write_dataset


def fixture():
    diagonal = [4.0 if i//5 == i%5 else 0.0 for i in range(25)]
    coupling = [-1.0 if i//5 == i%5 else 0.0 for i in range(25)]
    return [diagonal+[0.0]*25+coupling+[4.0]*5,
            diagonal+coupling+[0.0]*25+[7.0]*5]


class DatasetMathTests(unittest.TestCase):
    def test_oracle_satisfies_both_original_recurrences(self):
        raw = fixture()
        expected = oracle(raw)
        for lane in range(5):
            self.assertEqual(expected[0][lane], 1.0)
            self.assertEqual(expected[1][lane], 2.0)
            self.assertEqual(expected[0][5+lane], 1.5)
            self.assertEqual(expected[1][5+lane], 2.0)
        self.assertEqual(expected, oracle(raw,100))

    def test_oracle_handles_pivoting_instead_of_copying_guest_lu(self):
        raw = fixture()[:1]
        raw[0][50:75] = [0.0]*25
        raw[0][:25] = [float((i//5, i%5) in ((0,1),(1,0),(2,2),(3,3),(4,4))) for i in range(25)]
        raw[0][75:80] = [1.,2.,3.,4.,5.]
        self.assertEqual(oracle(raw), [[2.,1.,3.,4.,5.]*2])

    def test_market_counts_duplicates_and_rejects_invalid_values(self):
        text='%%MatrixMarket matrix coordinate real general\n% comment\n2 2 3\n1 1 1\n1 1 2\n2 2 4\n'
        self.assertEqual(market(io.StringIO(text)), (2,2,{(0,0):3.,(1,1):4.}))
        for broken in (text.replace('2 2 3','2 2 4'), text.replace('2 2 4\n','3 2 4\n'),
                       text.replace('1 1 2\n','1 1 nan\n')):
            with self.assertRaises(ValueError): market(io.StringIO(broken))

    def test_path_uses_real_edges_and_original_indices_without_repeats(self):
        eye=[float(i//5 == i%5) for i in range(25)]
        blocks={(i,i):eye for i in range(4)}
        for i,j in ((0,1),(0,3),(1,2),(2,3)):
            blocks[i,j]=blocks[j,i]=[v*(20 if j==3 else 1) for v in eye]
        adj,norms=graph(blocks,4)
        path=block_path(0,4,adj,norms)
        self.assertEqual(path,[0,1,2,3])
        raw=records_for_path(path,blocks,list(range(20)))
        self.assertEqual(raw[0][25:50],[0.]*25)
        self.assertEqual(raw[-1][50:75],[0.]*25)
        self.assertEqual(raw[2][75:80],[10,11,12,13,14])
        with self.assertRaises(ValueError):block_path(0,5,adj,norms)


class DatasetLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory()
        cls.root=Path(cls.directory.name)
        source=cls.root/'loader.c'
        include=ROOT/'benchmarks/lusgs/step2_dataset.inc'
        source.write_text(r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define N 5
typedef struct {size_t cells,lines; double *d_mat,*l_mat,*u_mat,*rhs,*dataset_oracle_y,*dataset_oracle_dq;} Problem;
typedef struct {const char *coeff_input_file;} Config;
static void *xcalloc(size_t n,size_t s,const char *name){(void)name;void *p=calloc(n,s);if(!p)exit(3);return p;}
static double *mat_at(double *a,const Problem *p,size_t l,size_t c){return a+(l*p->cells+c)*25;}
static double *vec_at(double *a,const Problem *p,size_t l,size_t c){return a+(l*p->cells+c)*5;}
'''+'\n#include "'+str(include)+'"\n'+r'''
int main(int argc,char **argv){
 Problem p={0};p.lines=1;p.cells=strtoul(argv[2],NULL,10);
 p.d_mat=calloc(p.cells*25,8);p.l_mat=calloc(p.cells*25,8);p.u_mat=calloc(p.cells*25,8);p.rhs=calloc(p.cells*5,8);
 double *records=aligned_alloc(64,p.cells*80*sizeof(double));
 if(!records)return 3;
 Config cfg={argv[1]};load_raw_dataset(&p,&cfg,records);
 for(size_t cell=0;cell<p.cells;++cell){
     const double *row=records+80*cell;
     if(memcmp(row,mat_at(p.d_mat,&p,0,cell),25*sizeof(double)) ||
        memcmp(row+25,mat_at(p.l_mat,&p,0,cell),25*sizeof(double)) ||
        memcmp(row+50,mat_at(p.u_mat,&p,0,cell),25*sizeof(double)) ||
        memcmp(row+75,vec_at(p.rhs,&p,0,cell),5*sizeof(double)))return 3;
 }
 free(records);
 printf("VALUES d0=%.17g rhs0=%.17g y0=%.17g dq0=%.17g\n",p.d_mat[0],p.rhs[0],p.dataset_oracle_y[0],p.dataset_oracle_dq[0]);
 if(argc>3)p.dataset_oracle_dq[0]+=1;
 return validate_dataset_oracle(&p,p.dataset_oracle_y,p.dataset_oracle_dq,"cpu")?0:1;
}
''')
        # Golden/output separation is tested by perturbing a separate copy below.
        text=source.read_text().replace('if(argc>3)p.dataset_oracle_dq[0]+=1;',
             'double *got=malloc(p.cells*5*8);memcpy(got,p.dataset_oracle_dq,p.cells*5*8);if(argc>3)got[0]+=1;').replace(
             'validate_dataset_oracle(&p,p.dataset_oracle_y,p.dataset_oracle_dq,"cpu")',
             'validate_dataset_oracle(&p,p.dataset_oracle_y,got,"cpu")')
        source.write_text(text)
        cls.binary=cls.root/'loader'
        subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-Wall','-Wextra','-Werror',str(source),'-lm','-o',str(cls.binary)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):cls.directory.cleanup()

    def invoke(self,data,cells=2,wrong=False):
        path=self.root/'input.cfd5';path.write_bytes(data)
        return subprocess.run([str(self.binary),str(path),str(cells)]+(['wrong'] if wrong else []),text=True,capture_output=True)

    def good(self):
        path=self.root/'good.cfd5'
        write_dataset(path,fixture())
        return path.read_bytes()

    def test_python_and_c_load_identical_coefficients_and_oracle(self):
        data=self.good();p=self.root/'roundtrip.cfd5';p.write_bytes(data)
        self.assertEqual(read_dataset(p)[0][:80],fixture()[0])
        result=self.invoke(data)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('VALUES d0=4 rhs0=4 y0=1 dq0=1.5',result.stdout)
        self.assertEqual(self.invoke(data,wrong=True).returncode,1)

    def test_c_and_python_reject_corruption_truncation_and_wrong_dimensions(self):
        data=self.good()
        for broken in (data[:20], data[:-1], data+b'X', data[:-1]+bytes([data[-1]^1])):
            result=self.invoke(broken)
            self.assertEqual(result.returncode,2,result.stdout+result.stderr)
            path=self.root/'bad.cfd5';path.write_bytes(broken)
            with self.assertRaises(ValueError):read_dataset(path)
        self.assertEqual(self.invoke(data,cells=3).returncode,2)

    def test_semantic_rejection_even_with_valid_checksum(self):
        data=self.good();header=list(HEADER.unpack_from(data))
        for offset,value in ((0,math.nan),(25,1.0),(90+50,1.0)):
            body=bytearray(data[HEADER.size:]);struct.pack_into('<d',body,offset*8,value)
            header[-1]=fnv64(body);broken=HEADER.pack(*header)+body
            self.assertEqual(self.invoke(broken).returncode,2)




class FrozenDatasetTests(unittest.TestCase):
    def test_identical_regeneration_keeps_existing_file_and_hardlink_unchanged(self):
        import os
        from single_line_dataset import write_dataset
        from prepare_single_line_study import stationary_records
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'data.cfd5';alias=Path(directory)/'alias.cfd5'
            raw=stationary_records(2);write_dataset(path,raw)
            alias.hardlink_to(path);os.utime(path,ns=(123456789,123456789))
            before=path.stat();write_dataset(path,raw)
            self.assertEqual(path.stat().st_mtime_ns,before.st_mtime_ns)
            self.assertEqual(alias.stat().st_ino,before.st_ino)
            self.assertEqual(alias.read_bytes(),path.read_bytes())

if __name__ == '__main__':
    unittest.main()
