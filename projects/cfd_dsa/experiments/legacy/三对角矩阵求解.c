/*******************************************************************************\
 Solve tri-diagonal block system
\*******************************************************************************/
template< size_t N >
void BlockLUSolveInplace( RealFlow (*DQ)[N], const RealFlow (*sub_matrix_A)[N][N], const RealFlow (*sub_matrix_B)[N][N], const RealFlow (*sub_matrix_C)[N][N], IntType nCellsInLine )
{
    //forward elimination for the tridiagonal block system
    for(IntType iv=0; iv<nCellsInLine-1; ++iv){
        //A^-1 dq
        //forward elimination
        for(int k=0; k<N - 1; ++k){
            //scale row k by A[k][k]
            DQ[iv][k] /= sub_matrix_A[iv][k][k];
            //loop over rows
            for(int i=k+1; i<N; ++i) DQ[iv][i] -= sub_matrix_A[iv][i][k] * DQ[iv][k];
        } //end of k

        //scale by A[4][4]
        DQ[iv][N-1] /= sub_matrix_A[iv][N-1][N-1];

        //end forward elimination && start backward substitution
        //loop over rows
        for(int k=N-2; k>=0; --k){ ///3
            for(int i=k+1; i<N; ++i){
                DQ[iv][k] -= sub_matrix_A[iv][k][i] * DQ[iv][i];
            }
        } //end backward substitution

        // compute dq[iv+1] := dq[iv+1] - C[iv+1] * dq[iv];
        for(int i=0; i<N; ++i){
            for(int k=0; k<N; ++k){
                DQ[iv+1][i] -= sub_matrix_C[iv+1][i][k] * DQ[iv][k];
            }
        }
    } //end of jCell iv

    //A^-1 dq at nCellsInLine-1
    //forward elimination
    for(int k=0; k<N - 1; ++k){
        //scale row k by A[k][k]
        DQ[nCellsInLine-1][k] /= sub_matrix_A[nCellsInLine-1][k][k];
        //loop over rows
        for(int i=k+1; i<N; ++i) DQ[nCellsInLine-1][i] -= sub_matrix_A[nCellsInLine-1][i][k] * DQ[nCellsInLine-1][k];
    } //end of k end forward elimination

    //scale by A[4][4]
    DQ[nCellsInLine-1][N-1] /= sub_matrix_A[nCellsInLine-1][N-1][N-1];

    //end forward elimination && start backward substitution
    //loop over rows
    for(int k=N-2; k>=0; --k){ ///3
        for(int i=k+1; i<N; ++i){
            DQ[nCellsInLine-1][k] -= sub_matrix_A[nCellsInLine-1][k][i] * DQ[nCellsInLine-1][i];
        }
    } //end backward substitution

    //backward substitution for the block tri-diagonal system
    //loop over block rows
    for(IntType iv=nCellsInLine-2; iv>=0; --iv){ ///-2
        //compute dq = dq- B dq[iv+1]
        for(int i=0; i<N; ++i){
            for(int k=0; k<N; ++k){
                DQ[iv][i] -= sub_matrix_B[iv][i][k] * DQ[iv+1][k];
            }
        }
    } //end of jCell iv
}