/*********************************************************************
 **
 ** file: rlm_anova.c
 **
 ** Aim: implement robust linear models specialized to samples + probes model.
 **
 ** Copyright (C) 2004-2009 Ben Bolstad
 **
 ** created by: B. M. Bolstad <bmb@bmbolstad.com>
 ** 
 ** created on: June 23, 2003
 **
 ** Last modified: June 23, 2003
 **
 ** History:
 ** July 29, 2004 - change routines so output order is the same as 
 **                 in new structure.
 ** Mar 1, 2006 - change comment style to ansi
 ** Apr 10, 2007 - add rlm_wfit_anova
 ** May 19, 2007 - branch out of affyPLM into a new package preprocessCore, then restructure the code. Add doxygen style documentation
 ** Mar 9. 2008 - Add rlm_fit_anova_given_probeeffects
 ** Mar 10, 2008 - make rlm_fit_anova_given_probeeffects etc purely single chip
 ** Mar 12, 2008 - Add rlm_wfit_anova_given_probeeffects
 ** Nov 1, 2008 - modify rlm_fit_anova_rlm_compute_se_anova() so that se of constrained probe effect (last one) is returned)
 ** Apr 23, 2009 - Allow scale estimate to be specified or returned in rlm_fit_anova
 ** Apr 24, 2009 - Allow scale estimate to be specified or returned in rlm_wfit_anova, rlm_fit_anova_given_probe_effects
 ** Apr 29, 2009 - Ensure that compute scale corresponds to final computed scale estimate
 **
 *********************************************************************/


#include "psi_fns.h"
#include "matrix_functions.h"
#include "rlm.h"
#include "rlm_se.h"

#include <R_ext/Rdynload.h>
#include <R.h>
#include <Rdefines.h>
#include <Rmath.h>
#include <Rinternals.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>



static void XTWY(int y_rows, int y_cols, double *wts,double *y, double *xtwy){

  int i,j;
  /* sweep columns (ie chip effects) */
   
  for (j=0; j < y_cols; j++){
    xtwy[j] = 0.0;
    for (i=0; i < y_rows; i++){
      xtwy[j] += wts[j*y_rows + i]* y[j*y_rows + i];
    }
  }
  
  /* sweep rows  (ie probe effects) */
 
  for (i=0; i < y_rows; i++){
    xtwy[i+y_cols] = 0.0;
    for (j=0; j < y_cols; j++){
      xtwy[i+y_cols] += wts[j*y_rows + i]* y[j*y_rows + i]; 
    }
  }

  for (i=0; i < y_rows-1; i++){
    xtwy[i+y_cols] = xtwy[i+y_cols] - xtwy[y_cols+y_rows-1];
  }
  
}




/**********************************************************************************
 **
 ** This is for testing the XTWY() function from R using .C()
 **
 *********************************************************************************/



void XTWY_R(int *rows, int *cols, double *out_weights, double *y,double *xtwy){
  XTWY(*rows, *cols, out_weights,y,xtwy);
}


/***************

This is R testing code for my own purposes


library(AffyExtensions)
data(Dilution)
  
y <- pm(Dilution)[1:16,]

.C("XTWY_R",as.integer(16),as.integer(4),as.double(rep(1,64)),as.double(as.vector(log2(y))),double(100))

probes <- rep(1:16,4)
samples <- rep(1:4,c(rep(16,4)))
X <- model.matrix(~-1 + as.factor(samples) + C(as.factor(probes),"contr.sum"))
t(X)%*%as.vector(log2(y))

****************/


static void XTWX(int y_rows, int y_cols, double *wts, double *xtwx){

  int Msize = y_cols +y_rows-1;
  int i,j,k;

  /* diagonal elements of first part of matrix ie upper partition */
  for (j =0; j < y_cols;j++){
    for (i=0; i < y_rows; i++){
      xtwx[j*Msize + j]+=wts[j*y_rows + i];
    }
  }


  /* diagonal portion of lower partition matrix: diagonal elements*/ 
  for (j =0; j < y_cols;j++){
    for (i = 0; i < y_rows-1;i++){
      xtwx[(y_cols +i)*Msize + (y_cols +i)]+= wts[j*y_rows + i];
    }
  }
  /* diagonal portion of lower partition matrix: off diagonal elements*/ 
  for (j =0; j < y_cols;j++){
    for (i = 0; i < y_rows-1;i++){
      for (k=i ; k <  y_rows-1;k++){
	xtwx[(y_cols +k)*Msize + (y_cols +i)] = xtwx[(y_cols +i)*Msize + (y_cols +k)]+= wts[j*y_rows + (y_rows-1)];
      }
    }
  }

  /* the two other portions of the matrix */
  for (j =0; j < y_cols;j++){
    for (i= 0; i < y_rows-1;i++){
       xtwx[j*Msize + (y_cols + i)] = xtwx[(y_cols + i)*Msize + j] = wts[j*y_rows + i] - wts[j*y_rows + (y_rows-1)];
    }
  }

}



/**********************************************************************************
 **
 ** This is for testing the XTWX from R using .C()
 **
 *********************************************************************************/

void XTWX_R(int *rows, int *cols, double *out_weights, double *xtwx){

  XTWX(*rows, *cols, out_weights,xtwx);
}

/***************

This is R test code

.C("XTWX_R",as.integer(16),as.integer(4),rep(1,64))



*************/

static void XTWXinv(int y_rows, int y_cols,double *xtwx){
  int i,j,k;
  int Msize = y_cols +y_rows-1;
  double *P= R_Calloc(y_cols,double);
  double *RP = R_Calloc(y_cols*(y_rows-1),double);
  double *RPQ = R_Calloc((y_rows-1)*(y_rows-1),double);
  double *S = R_Calloc((y_rows-1)*(y_rows-1),double);
  double *work = R_Calloc((y_rows-1)*(y_rows-1),double);
  
  for (j=0;j < y_cols;j++){
    for (i=0; i < y_rows -1; i++){
      RP[j*(y_rows-1) + i] = xtwx[j*Msize + (y_cols + i)]*(1.0/xtwx[j*Msize+j]);
    }
  } 
  
  for (i=0; i < y_rows -1; i++){
    for (j=i;j <  y_rows -1; j++){
      for (k=0; k < y_cols;k++){
	RPQ[j*(y_rows-1) + i] +=  RP[k*(y_rows-1) + j]*xtwx[k*Msize + (y_cols + i)];
      }
      RPQ[i*(y_rows-1) + j] = RPQ[j*(y_rows-1) + i];
    }
  }
  
  for (j=0; j < y_rows-1;j++){
    for (i=j; i < y_rows-1;i++){
      RPQ[i*(y_rows-1) + j] = RPQ[j*(y_rows-1)+i] =  xtwx[(y_cols + j)*Msize + (y_cols + i)] - RPQ[j*(y_rows-1) + i]; 
    }
  } 
  
  
  /*for (i =0; i<  y_rows-1; i++){
    for (j=0; j <  y_cols; j++){ 
      printf("%4.4f ",RP[j*(y_rows-1) + i]);
    }
    printf("\n");
    }
  
    for (j=0;j <  y_rows -1; j++){
    for (i=0; i < y_rows -1; i++){
    printf("%4.4f ",RPQ[j*(y_rows-1) + i]);
    }
    printf("\n");
    }
  

    for (i=0; i < y_rows -1; i++){
    for (j=0;j <  y_rows -1; j++){
    printf("%4.4f ",S[j*(y_rows-1) + i]);
    }
    printf("\n");
    }
  */
  



  /* Lets start making the inverse */

  Choleski_inverse(RPQ, S, work, y_rows-1, 0);

  for (j=0; j< y_cols;j++){
    for (i=0; i < y_rows -1; i++){
      xtwx[j*Msize + (y_cols + i)] = 0.0;
      for (k=0; k < y_rows -1; k++){
	xtwx[j*Msize + (y_cols + i)]+= -1.0*(S[i*(y_rows-1) + k])*RP[j*(y_rows-1) + k];
      }
      xtwx[(y_cols + i)*Msize + j]=xtwx[j*Msize + (y_cols + i)];
    }
  }


  for (j=0;j < y_cols;j++){
      P[j] = 1.0/xtwx[j*Msize+j];
  } 


  for (j=0; j < y_cols; j++){
    for (i=j; i < y_cols;i++){
      xtwx[i*Msize + j]=0.0;
      for (k=0;k < y_rows-1; k++){
	xtwx[i*Msize + j]+= RP[i*(y_rows-1) + k]*xtwx[j*Msize + (y_cols + k)];
      }
      xtwx[i*Msize + j]*=-1.0;
      xtwx[j*Msize + i] =  xtwx[i*Msize + j];
    }
    xtwx[j*Msize + j]+=P[j];
  }


  for (j=0; j < y_rows-1;j++){
    for (i=0; i < y_rows-1;i++){
      xtwx[(y_cols + j)*Msize + (y_cols + i)] = S[j*(y_rows-1)+i];
    }
  }


  R_Free(P);
  R_Free(work);
  R_Free(RP);
  R_Free(RPQ);
  R_Free(S);

}


/**********************************************************************************
 **
 ** This is for testing the XTWXinv from R
 **
 *********************************************************************************/

void XTWX_R_inv(int *rows, int *cols, double *xtwx){

  XTWXinv(*rows, *cols, xtwx);
}


/***************

This is R testing code for my own purposes

library(AffyExtensions)

probes <- rep(1:16,4)
 samples <- rep(1:4,c(rep(16,4)))


X <- model.matrix(~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"))

W <- diag(seq(0.05,1,length=64))




solve(t(X)%*%W%*%X) - matrix(.C("XTWX_R_inv",as.integer(16),as.integer(4),as.double(t(X)%*%W%*%X))[[3]],19,19)

matrix(.C("XTWX_R_inv",as.integer(16),as.integer(4),as.double(t(X)%*%W%*%X))[[3]],19,19)[1:4,5:19]

XTWX <- t(X)%*%W%*%X

R <- XTWX[5:19,1:4]
P <- XTWX[1:4,1:4]
Q <- t(R)
S <- XTWX [5:19,5:19]


R%*%solve(P)

 probes <- rep(1:16,100)
  samples <- rep(1:100,c(rep(16,100)))


 X <- model.matrix(~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"))

  W <- diag(seq(0.05,1,length=1600))

 system.time(matrix(.C("XTWX_R_inv",as.integer(16),as.integer(100),as.double(t(X)%*%W%*%X))[[3]],115,115))

*************/




static void rlm_fit_anova_engine(double *y, int y_rows, int y_cols, double *input_scale, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){


  int i,j,iter;
  /* double tol = 1e-7; */
  double acc = 1e-4;
  double scale =0.0;
  double conv;
  double endprobe;

  double *wts = out_weights; 

  double *resids = out_resids; 
  double *old_resids = R_Calloc(y_rows*y_cols,double);
  
  double *rowmeans = R_Calloc(y_rows,double);

  double *xtwx = R_Calloc((y_rows+y_cols-1)*(y_rows+y_cols-1),double);
  double *xtwy = R_Calloc((y_rows+y_cols),double);

  double sumweights, rows;
  
  rows = y_rows*y_cols;
  
  if (!initialized){
    
    /* intially use equal weights */
    for (i=0; i < rows; i++){
      wts[i] = 1.0;
    }
  }

  /* starting matrix */
  
  for (i=0; i < y_rows; i++){
    for (j=0; j < y_cols; j++){
      resids[j*y_rows + i] = y[j*y_rows + i];
    }
  }
  
  /* sweep columns (ie chip effects) */

  for (j=0; j < y_cols; j++){
    out_beta[j] = 0.0;
    sumweights = 0.0;
    for (i=0; i < y_rows; i++){
      out_beta[j] += wts[j*y_rows + i]* resids[j*y_rows + i];
      sumweights +=  wts[j*y_rows + i];
    }
    out_beta[j]/=sumweights;
    for (i=0; i < y_rows; i++){
      resids[j*y_rows + i] = resids[j*y_rows + i] -  out_beta[j];
    }
  }


 /* sweep rows  (ie probe effects) */
  
  for (i=0; i < y_rows; i++){
    rowmeans[i] = 0.0;
    sumweights = 0.0;
    for (j=0; j < y_cols; j++){
      rowmeans[i] += wts[j*y_rows + i]* resids[j*y_rows + i]; 
      sumweights +=  wts[j*y_rows + i];
    }
    rowmeans[i]/=sumweights;
    for (j=0; j < y_cols; j++){
       resids[j*y_rows + i] =  resids[j*y_rows + i] - rowmeans[i];
    }
  }
  for (i=0; i < y_rows-1; i++){
    out_beta[i+y_cols] = rowmeans[i];
  }



  for (iter = 0; iter < max_iter; iter++){
    if (*input_scale < 0){
      scale = med_abs(resids,rows)/0.6745;
    } else {
      scale = *input_scale;
    }


    if (fabs(scale) < 1e-10){
      /*printf("Scale too small \n"); */
      break;
    }
    for (i =0; i < rows; i++){
      old_resids[i] = resids[i];
    }

    for (i=0; i < rows; i++){
      wts[i] = PsiFn(resids[i]/scale,psi_k,0);  /*           psi_huber(resids[i]/scale,k,0); */
    }
   
    /* printf("%f\n",scale); */


    /* weighted least squares */
    
    memset(xtwx,0,(y_rows+y_cols-1)*(y_rows+y_cols-1)*sizeof(double));


    XTWX(y_rows,y_cols,wts,xtwx);
    XTWXinv(y_rows, y_cols,xtwx);
    XTWY(y_rows, y_cols, wts,y, xtwy);

    
    for (i=0;i < y_rows+y_cols-1; i++){
      out_beta[i] = 0.0;
       for (j=0;j < y_rows+y_cols -1; j++){
    	 out_beta[i] += xtwx[j*(y_rows+y_cols -1)+i]*xtwy[j];
       }
    }

    /* residuals */
    
    for (i=0; i < y_rows-1; i++){
      for (j=0; j < y_cols; j++){
	resids[j*y_rows +i] = y[j*y_rows + i]- (out_beta[j] + out_beta[i + y_cols]); 
      }
    }

    for (j=0; j < y_cols; j++){
      endprobe=0.0;
      for (i=0; i < y_rows-1; i++){
	endprobe+= out_beta[i + y_cols];
      }
      resids[j*y_rows + y_rows-1] = y[j*y_rows + y_rows-1]- (out_beta[j] - endprobe);
    }

    /*check convergence  based on residuals */
    
    conv = irls_delta(old_resids,resids, rows);
    
    if (conv < acc){
      /*    printf("Converged \n");*/
      break; 

    }
  }
    
  if (*input_scale < 0){
    scale = med_abs(resids,rows)/0.6745;
  } else {
    scale = *input_scale;
  }

  R_Free(xtwx);
  R_Free(xtwy);
  R_Free(old_resids);
  R_Free(rowmeans);
  input_scale[0] = scale;

}



void rlm_fit_anova_scale(double *y, int y_rows, int y_cols,double *scale, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){
  
  rlm_fit_anova_engine(y, y_rows, y_cols, scale, out_beta, out_resids, out_weights,PsiFn, psi_k, max_iter, initialized);
}




/**********************************************************************************
 **
 ** void rlm_fit_anova(double *y, int rows, int cols,double *out_beta, 
 **                double *out_resids, double *out_weights,
 **                double (* PsiFn)(double, double, int), double psi_k,int max_iter, 
 **                int initialized))
 **
 ** double *y - matrix of response variables (stored by column, with rows probes, columns chips
 ** int rows - dimensions of y
 ** int cols - dimensions of y
 **
 ** specializes procedure so decomposes matrix more efficiently
 ** note that routine is not as numerically stable as above.
 **
 ** fits a row + columns model
 **
 **********************************************************************************/

void rlm_fit_anova(double *y, int y_rows, int y_cols,double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){
  
  double scale = -1.0;
  rlm_fit_anova_engine(y, y_rows, y_cols, &scale, out_beta, out_resids, out_weights,PsiFn, psi_k, max_iter, initialized);
}


/**********************************************************************************
 **
 ** This is for testing the rlm_fit_anova function from R
 **
 *********************************************************************************/

void rlm_fit_anova_R(double *y, int *rows, int *cols, double *out_beta, double *out_resids, double *out_weights, int *its){

  rlm_fit_anova(y, *rows, *cols, out_beta, out_resids,out_weights,psi_huber,1.345, *its,0);
}


/*    

This is testing code for my own use.

library(AffyExtensions)
data(Dilution)
  
y <- pm(Dilution)[1:16,]


.C("rlm_fit_anova_R",as.double(log2(y)),as.integer(16),as.integer(4),double(20),double(64),double(64),as.integer(1))

 probes <- rep(1:16,4)
 samples <- rep(1:4,c(rep(16,4)))

library(MASS)
rlm(as.vector(log2(y)) ~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"),maxit=20)$weights
# apply(matrix(rlm(as.vector(y) ~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"),maxit=1)$w,ncol=4)*y,2,sum)/apply(matrix(rlm(as.vector(y) ~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"),maxit=1)$w,ncol=4),2,sum)

abs(resid(rlm(as.vector(log2(y)) ~ -1 + as.factor(samples) + C(as.factor(probes),"contr.sum"),maxit=1))- .C("rlm_fit_anova_R",as.double(log2(y)),as.integer(16),as.integer(4),double(20),double(64),double(64),as.integer(1))[[5]]) > 10^-6



*/


void rlm_wfit_anova_engine(double *y, int y_rows, int y_cols, double *input_scale, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){

  int i,j,iter;
  /* double tol = 1e-7; */
  double acc = 1e-4;
  double scale =0.0;
  double conv;
  double endprobe;

  double *wts = out_weights; 

  double *resids = out_resids; 
  double *old_resids = R_Calloc(y_rows*y_cols,double);
  
  double *rowmeans = R_Calloc(y_rows,double);

  double *xtwx = R_Calloc((y_rows+y_cols-1)*(y_rows+y_cols-1),double);
  double *xtwy = R_Calloc((y_rows+y_cols),double);

  double sumweights, rows;
  
  rows = y_rows*y_cols;
  
  if (!initialized){
    
    /* intially use equal weights */
    for (i=0; i < rows; i++){
      wts[i] = w[i]*1.0;
    }
  }

  /* starting matrix */
  
  for (i=0; i < y_rows; i++){
    for (j=0; j < y_cols; j++){
      resids[j*y_rows + i] = y[j*y_rows + i];
    }
  }
  
  /* sweep columns (ie chip effects) */

  for (j=0; j < y_cols; j++){
    out_beta[j] = 0.0;
    sumweights = 0.0;
    for (i=0; i < y_rows; i++){
      out_beta[j] += wts[j*y_rows + i]* resids[j*y_rows + i];
      sumweights +=  wts[j*y_rows + i];
    }
    out_beta[j]/=sumweights;
    for (i=0; i < y_rows; i++){
      resids[j*y_rows + i] = resids[j*y_rows + i] -  out_beta[j];
    }
  }


 /* sweep rows  (ie probe effects) */
  
  for (i=0; i < y_rows; i++){
    rowmeans[i] = 0.0;
    sumweights = 0.0;
    for (j=0; j < y_cols; j++){
      rowmeans[i] += wts[j*y_rows + i]* resids[j*y_rows + i]; 
      sumweights +=  wts[j*y_rows + i];
    }
    rowmeans[i]/=sumweights;
    for (j=0; j < y_cols; j++){
       resids[j*y_rows + i] =  resids[j*y_rows + i] - rowmeans[i];
    }
  }
  for (i=0; i < y_rows-1; i++){
    out_beta[i+y_cols] = rowmeans[i];
  }



  for (iter = 0; iter < max_iter; iter++){
    if (*input_scale < 0){
      scale = med_abs(resids,rows)/0.6745;
    } else {
      scale = *input_scale;
    }
    
    if (fabs(scale) < 1e-10){
      /*printf("Scale too small \n"); */
      break;
    }
    for (i =0; i < rows; i++){
      old_resids[i] = resids[i];
    }

    for (i=0; i < rows; i++){
      wts[i] = w[i]*PsiFn(resids[i]/scale,psi_k,0);  /*           psi_huber(resids[i]/scale,k,0); */
    }
   
    /* printf("%f\n",scale); */


    /* weighted least squares */
    
    memset(xtwx,0,(y_rows+y_cols-1)*(y_rows+y_cols-1)*sizeof(double));


    XTWX(y_rows,y_cols,wts,xtwx);
    XTWXinv(y_rows, y_cols,xtwx);
    XTWY(y_rows, y_cols, wts,y, xtwy);

    
    for (i=0;i < y_rows+y_cols-1; i++){
      out_beta[i] = 0.0;
       for (j=0;j < y_rows+y_cols -1; j++){
    	 out_beta[i] += xtwx[j*(y_rows+y_cols -1)+i]*xtwy[j];
       }
    }

    /* residuals */
    
    for (i=0; i < y_rows-1; i++){
      for (j=0; j < y_cols; j++){
	resids[j*y_rows +i] = y[j*y_rows + i]- (out_beta[j] + out_beta[i + y_cols]); 
      }
    }

    for (j=0; j < y_cols; j++){
      endprobe=0.0;
      for (i=0; i < y_rows-1; i++){
	endprobe+= out_beta[i + y_cols];
      }
      resids[j*y_rows + y_rows-1] = y[j*y_rows + y_rows-1]- (out_beta[j] - endprobe);
    }

    /*check convergence  based on residuals */
    
    conv = irls_delta(old_resids,resids, rows);
    
    if (conv < acc){
      /*    printf("Converged \n");*/
      break; 

    }



  }
        
  if (*input_scale < 0){
    scale = med_abs(resids,rows)/0.6745;
  } else {
    scale = *input_scale;
  }

  R_Free(xtwx);
  R_Free(xtwy);
  R_Free(old_resids);
  R_Free(rowmeans);
  input_scale[0] = scale;

}





void rlm_wfit_anova(double *y, int y_rows, int y_cols, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){

  double scale = -1.0;
  rlm_wfit_anova_engine(y, y_rows, y_cols, &scale, w, out_beta, out_resids, out_weights, PsiFn , psi_k, max_iter, initialized);

}





void rlm_wfit_anova_scale(double *y, int y_rows, int y_cols,double *scale, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){
  
  rlm_wfit_anova_engine(y, y_rows, y_cols, scale, w, out_beta, out_resids, out_weights,PsiFn, psi_k, max_iter, initialized);
}






/*************************************************************************
 **
 ** void RLM_SE_Method_1_anova(double residvar, double *XTX, int p, double *se_estimates)
 **
 ** double residvar - residual variance estimate
 ** double *XTX - t(Design matrix)%*% Design Matrix
 ** double p - number of parameters
 ** double *se_estimates - on output contains standard error estimates for each of
 **                        the parametes
 **
 ** this function computes the parameter standard errors using the first
 ** method described in Huber (1981)
 ** 
 ** ie k^2 (sum psi^2/(n-p))/(sum psi'/n)^2 *(XtX)^(-1)
 **
 **
 ************************************************************************/


static void RLM_SE_Method_1_anova(double residvar, double *XTX, int y_rows,int y_cols, double *se_estimates,double *varcov){
  int i,j;
  int p = y_rows + y_cols -1;
  
  XTWXinv(y_rows, y_cols,XTX);


  for (i =0; i < p; i++){
    se_estimates[i] = sqrt(residvar*XTX[i*p + i]);
  }
  

  /*** for (i =0; i < y_rows-1; i++){
       se_estimates[i] = sqrt(residvar*XTX[(i+y_cols)*p + (i+y_cols)]);
       }
       for (i =0; i < y_cols; i++){
       se_estimates[i+(y_rows -1)] = sqrt(residvar*XTX[i*p + i]);
       }  ***/

  if (varcov != NULL)
    for (i =0; i < p; i++){
      for (j = i; j < p; j++){
	varcov[j*p +i]= residvar*XTX[j*p +i];
      }
    }
  

  /*** if (varcov != NULL){
       // copy across varcov matrix in right order 
       for (i = 0; i < y_rows-1; i++)
       for (j = i; j < y_rows-1; j++)
       varcov[j*p + i] =  residvar*XTX[(j+y_cols)*p + (i+y_cols)];
       
       for (i = 0; i < y_cols; i++)
       for (j = i; j < y_cols; j++)
       varcov[(j+(y_rows-1))*p + (i+(y_rows -1))] =  residvar*XTX[j*p + i];
       
       
       
       for (i = 0; i < y_cols; i++)
       for (j = y_cols; j < p; j++)
       varcov[(i+ y_rows -1)*p + (j - y_cols)] =  residvar*XTX[j*p + i];
       
       }  **/

}



/*************************************************************************
 **
 ** void RLM_SE_Method_2(double residvar, double *W, int p, double *se_estimates)
 **
 ** double residvar - residual variance estimate
 ** double *XTX - t(Design matrix)%*% Design Matrix
 ** double p - number of parameters
 ** double *se_estimates - on output contains standard error estimates for each of
 **                        the parametes
 **
 ** this function computes the parameter standard errors using the second
 ** method described in Huber (1981)
 ** 
 ** ie K*(sum psi^2/(n-p))/(sum psi'/n) *(W)^(-1)
 **
 **
 ************************************************************************/

static void RLM_SE_Method_2_anova(double residvar, double *W, int y_rows,int y_cols, double *se_estimates,double *varcov){
  int i,j; /* l,k; */
  int p = y_rows + y_cols -1;
  double *Winv = R_Calloc(p*p,double);
  double *work = R_Calloc(p*p,double);
  
  
  if (!Choleski_inverse(W,Winv,work,p,1)){
   for (i =0; i < p; i++){
      se_estimates[i] = sqrt(residvar*Winv[i*p + i]);
      /* printf("%f ", se_estimates[i]); */
    }
   /*for (i =0; i < y_rows-1; i++){
      se_estimates[i] = sqrt(residvar*Winv[(i+y_cols)*p + (i+y_cols)]);
      }
      for (i =0; i < y_cols; i++){
      se_estimates[i+(y_rows -1)] = sqrt(residvar*Winv[i*p + i]);
      } */
  } else {
    /* printf("Using a G-inverse\n"); */
    SVD_inverse(W, Winv,p);
    for (i =0; i < p; i++){
      se_estimates[i] = sqrt(residvar*Winv[i*p + i]);
      /* printf("%f ", se_estimates[i]); */
    }
    /* for (i =0; i < y_rows-1; i++){
       se_estimates[i] = sqrt(residvar*Winv[(i+y_cols)*p + (i+y_cols)]);
       }
       for (i =0; i < y_cols; i++){
       se_estimates[i+(y_rows -1)] = sqrt(residvar*Winv[i*p + i]);
       } */
  }
  
  if (varcov != NULL)
    for (i =0; i < p; i++){
      for (j = i; j < p; j++){
	varcov[j*p +i]= residvar*Winv[j*p +i];
      }
    }
  /** if (varcov != NULL){
       copy across varcov matrix in right order 
      for (i = 0; i < y_rows-1; i++)
      for (j = i; j < y_rows-1; j++)
      varcov[j*p + i] =  residvar*Winv[(j+y_cols)*p + (i+y_cols)];
      
      for (i = 0; i < y_cols; i++)
      for (j = i; j < y_cols; j++)
      varcov[(j+(y_rows-1))*p + (i+(y_rows -1))] =  residvar*Winv[j*p + i];
      
      
      
      for (i = 0; i < y_cols; i++)
      for (j = y_cols; j < p; j++)
      varcov[(i+ y_rows -1)*p + (j - y_cols)] =  residvar*Winv[j*p + i];
      } **/
  
  R_Free(work);
  R_Free(Winv);

}



/*************************************************************************
 **
 ** void RLM_SE_Method_3(double residvar, double *XTX, double *W, int p, double *se_estimates)
 **
 ** double residvar - residual variance estimate
 ** double *XTX - t(Design matrix)%*% Design Matrix
 ** double p - number of parameters
 ** double *se_estimates - on output contains standard error estimates for each of
 **                        the parametes
 **
 ** this function computes the parameter standard errors using the third
 ** method described in Huber (1981)
 ** 
 ** ie 1/(K)*(sum psi^2/(n-p))*(W)^(-1)(XtX)W^(-1)
 **
 **
 ************************************************************************/

static int RLM_SE_Method_3_anova(double residvar, double *XTX, double *W,  int y_rows,int y_cols, double *se_estimates,double *varcov){
  int i,j,k;   /* l; */
  int rv;
  int p = y_rows + y_cols -1;
  double *Winv = R_Calloc(p*p,double);
  double *work = R_Calloc(p*p,double);
  

  /***************** 

  double *Wcopy = R_Calloc(p*p,double);

  
  for (i=0; i <p; i++){
    for (j=0; j < p; j++){
      Wcopy[j*p + i] = W[j*p+i];
    }
    } **********************/
  
  if(Choleski_inverse(W,Winv,work,p,1)){
    SVD_inverse(W, Winv,p);
  }
  
  /*** want W^(-1)*(XtX)*W^(-1) ***/

  /*** first Winv*(XtX) ***/

  for (i=0; i <p; i++){
    for (j=0; j < p; j++){
      work[j*p + i] = 0.0;
      for (k = 0; k < p; k++){
	work[j*p+i]+= Winv[k*p +i] * XTX[j*p + k];
      }
    }
  }
 
  /* now again by W^(-1) */
  
   for (i=0; i <p; i++){
    for (j=0; j < p; j++){
      W[j*p + i] =0.0;
      for (k = 0; k < p; k++){
	W[j*p+i]+= work[k*p +i] * Winv[j*p + k];
      }
    }
   }
     

   /* make sure in right order */

   /*  for (i =0; i < y_rows-1; i++){
       se_estimates[i] = sqrt(residvar*W[(i+y_cols)*p + (i+y_cols)]);
       }
       for (i =0; i < y_cols; i++){
       se_estimates[i+(y_rows -1)] = sqrt(residvar*W[i*p + i]);
       }*/
   
   for (i =0; i < p; i++){
     se_estimates[i] = sqrt(residvar*W[i*p + i]);
     /*  printf("%f ", se_estimates[i]); */
   }

   rv = 0;
   
   if (varcov != NULL)
     for (i =0; i < p; i++){
       for (j = i; j < p; j++){
	 varcov[j*p +i]= residvar*W[j*p +i];
       }
     }

   /* if (varcov != NULL){
       copy across varcov matrix in right order 
      for (i = 0; i < y_rows-1; i++)
      for (j = i; j < y_rows-1; j++)
      varcov[j*p + i] =  residvar*W[(j+y_cols)*p + (i+y_cols)];
      
      for (i = 0; i < y_cols; i++)
      for (j = i; j < y_cols; j++)
      varcov[(j+(y_rows-1))*p + (i+(y_rows -1))] =  residvar*W[j*p + i];
      
      
      
      for (i = 0; i < y_cols; i++)
      for (j = y_cols; j < p; j++)
      varcov[(i+ y_rows -1)*p + (j - y_cols)] =  residvar*W[j*p + i];
      } */
   R_Free(work);
   R_Free(Winv);

   return rv;

}





/*********************************************************************
 **
 ** void rlm_compute_se(double *X,double *Y, int n, int p, double *beta, double *resids,double *weights,double *se_estimates, int method)
 **
 ** specializes to the probes + arrays model the method for computing the standard errors of the parameter estimates
 **
 *********************************************************************/

void rlm_compute_se_anova(double *Y, int y_rows,int y_cols, double *beta, double *resids,double *weights,double *se_estimates, double *varcov, double *residSE, int method,double (* PsiFn)(double, double, int), double psi_k){
  
  int i,j; /* counter/indexing variables */
  double k1 = psi_k;   /*  was 1.345; */
  double sumpsi2=0.0;  /* sum of psi(r_i)^2 */
  /*  double sumpsi=0.0; */
  double sumderivpsi=0.0; /* sum of psi'(r_i) */
  double Kappa=0.0;      /* A correction factor */
  double scale=0.0;
  int n = y_rows*y_cols;
  int p = y_rows + y_cols -1;
  double *XTX = R_Calloc(p*p,double);
  double *W = R_Calloc(p*p,double);
  double *work = R_Calloc(p*p,double);
  double RMSEw = 0.0;
  double vs=0.0,m,varderivpsi=0.0; 
  double *W_tmp=R_Calloc(n,double);


  if (method == 4){
    for (i=0; i < n; i++){
      RMSEw+= weights[i]*resids[i]*resids[i];
    }
    
    RMSEw = sqrt(RMSEw/(double)(n-p));

    residSE[0] =  RMSEw;


    XTWX(y_rows,y_cols,weights,XTX);
    if (y_rows > 1){
      XTWXinv(y_rows, y_cols,XTX);
    } else {
      for (i=0; i < p; i++){
	XTX[i*p + i] = 1.0/XTX[i*p + i];
      }
    }
    /* make sure in right order 
       
    for (i =0; i < y_rows-1; i++){
    se_estimates[i] = RMSEw*sqrt(XTX[(i+y_cols)*p + (i+y_cols)]);
    }
    for (i =0; i < y_cols; i++){
    se_estimates[i+(y_rows -1)] = RMSEw*sqrt(XTX[i*p + i]);
    } */
    
    for (i =0; i < p; i++){
      se_estimates[i] = RMSEw*sqrt(XTX[i*p + i]);
    }
    
    
    if (varcov != NULL)
      for (i = 0; i < p; i++)
	for (j = i; j < p; j++)
	  varcov[j*p + i] =  RMSEw*RMSEw*XTX[j*p + i];
    
    se_estimates[p] = 0.0;

    for (i=y_cols; i < p; i++)
      for (j = y_cols; j < p; j++)
    se_estimates[p]+= -1*RMSEw*RMSEw*XTX[j*p + i];
    
    se_estimates[p] = sqrt(-1*se_estimates[p]);

    /*     if (varcov != NULL){
	   copy across varcov matrix in right order 
	   for (i = 0; i < y_rows-1; i++)
	   for (j = i; j < y_rows-1; j++)
	   varcov[j*p + i] =  RMSEw*RMSEw*XTX[(j+y_cols)*p + (i+y_cols)];
	   
	   for (i = 0; i < y_cols; i++)
	   for (j = i; j < y_cols; j++)
	   varcov[(j+(y_rows-1))*p + (i+(y_rows -1))] =  RMSEw*RMSEw*XTX[j*p + i];
	   
	   
      
	   for (i = 0; i < y_cols; i++)
	   for (j = y_cols; j < p; j++)
	   varcov[(i+ y_rows -1)*p + (j - y_cols)] =  RMSEw*RMSEw*XTX[j*p + i];
	   } */


  } else {
    scale = med_abs(resids,n)/0.6745;
    
    residSE[0] =  scale;
    
    /* compute most of what we will need to do each of the different standard error methods */
    for (i =0; i < n; i++){
      sumpsi2+= PsiFn(resids[i]/scale,k1,2)*PsiFn(resids[i]/scale,k1,2); 
      /* sumpsi += psi_huber(resids[i]/scale,k1,2); */
      sumderivpsi+= PsiFn(resids[i]/scale,k1,1);
    }
    
    m = (sumderivpsi/(double) n);

    for (i = 0; i < n; i++){
      varderivpsi+=(PsiFn(resids[i]/scale,k1,1) - m)*(PsiFn(resids[i]/scale,k1,1) - m);
    }
    varderivpsi/=(double)(n);

    /*    Kappa = 1.0 + (double)p/(double)n * (1.0-m)/(m); */


    Kappa = 1.0 + ((double)p/(double)n) *varderivpsi/(m*m);

    
    /* prepare XtX and W matrices */

    for (i=0; i < n; i++){
      W_tmp[i] = 1.0;
    }
    XTWX(y_rows,y_cols,W_tmp,XTX);
    
     for (i=0; i < n; i++){
       W_tmp[i] = PsiFn(resids[i]/scale,k1,1);
    }
    XTWX(y_rows,y_cols,W_tmp,W);

    if (method==1) {
      Kappa = Kappa*Kappa;
      vs = scale*scale*sumpsi2/(double)(n-p);
      Kappa = Kappa*vs/(m*m);
      RLM_SE_Method_1_anova(Kappa, XTX, y_rows,y_cols, se_estimates,varcov);
    } else if (method==2){
      vs = scale*scale*sumpsi2/(double)(n-p);
      Kappa = Kappa*vs/m;
      RLM_SE_Method_2_anova(Kappa, W, y_rows,y_cols, se_estimates,varcov);
      
    } else if (method==3){
      
      vs = scale*scale*sumpsi2/(double)(n-p);
      Kappa = 1.0/Kappa*vs;
      i = RLM_SE_Method_3_anova(Kappa, XTX, W, y_rows,y_cols, se_estimates,varcov);
      if (i){
	for (i=0; i <n; i++){
	  //	  printf("%2.1f ", PsiFn(resids[i]/scale,k1,1));
	} 
	//printf("\n");
      }
    } 
  }
  R_Free(W_tmp);
  R_Free(work);
  R_Free(XTX);
  R_Free(W);

}

















static void colonly_XTWX(int y_rows, int y_cols, double *wts, double *xtwx){

  int Msize = y_cols;
  int i,j;

  /* diagonal elements of first part of matrix ie upper partition */
  for (j =0; j < y_cols;j++){
    for (i=0; i < y_rows; i++){
      xtwx[j*Msize + j]+=wts[j*y_rows + i];
    }
  }
}


static void colonly_XTWXinv(int y_rows, int y_cols,double *xtwx){
  int j;
  int Msize = y_cols;

  for (j=0;j < y_cols;j++){
    xtwx[j*Msize+j]= 1.0/xtwx[j*Msize+j];
  } 
}




static void colonly_XTWY(int y_rows, int y_cols, double *wts,double *y, double *xtwy){

  int i,j;
  /* sweep columns (ie chip effects) */
   
  for (j=0; j < y_cols; j++){
    xtwy[j] = 0.0;
    for (i=0; i < y_rows; i++){
      xtwy[j] += wts[j*y_rows + i]* y[j*y_rows + i];
    }
  }
}



void rlm_fit_anova_given_probe_effects_engine(double *y, int y_rows, int y_cols, double *input_scale, double *probe_effects, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){

  int i,j,iter;
  /* double tol = 1e-7; */
  double acc = 1e-4;
  double *scale =R_Calloc((y_cols),double);;
  double conv;
  double endprobe;

  double *wts = out_weights; 

  double *resids = out_resids; 
  double *old_resids = R_Calloc(y_rows*y_cols,double);
  
  double *rowmeans = R_Calloc(y_rows,double);

  double *xtwx = R_Calloc((y_cols)*(y_cols),double);
  double *xtwy = R_Calloc((y_cols),double);

  double sumweights, rows;
  
  rows = y_rows*y_cols;
  
  if (!initialized){
    
    /* intially use equal weights */
    for (i=0; i < rows; i++){
      wts[i] = 1.0;
    }
  }

  /* starting matrix */
  
  for (i=0; i < y_rows; i++){
    for (j=0; j < y_cols; j++){
      resids[j*y_rows + i] = y[j*y_rows + i] - probe_effects[i];
    }
  }
  
  /* sweep columns (ie chip effects) */

  for (j=0; j < y_cols; j++){
    out_beta[j] = 0.0;
    sumweights = 0.0;
    for (i=0; i < y_rows; i++){
      out_beta[j] += wts[j*y_rows + i]* resids[j*y_rows + i];
      sumweights +=  wts[j*y_rows + i];
    }
    out_beta[j]/=sumweights;
    for (i=0; i < y_rows; i++){
      resids[j*y_rows + i] = resids[j*y_rows + i] -  out_beta[j];
    }
  }
  
  for (iter = 0; iter < max_iter; iter++){
    
    /* The new single-chip code */

    for (i =0; i < rows; i++){
      old_resids[i] = resids[i];
    }


    for (j = 0; j < y_cols; j++){
      if (input_scale[j] < 0.0){
	scale[j] = med_abs(&resids[j*y_rows],y_rows)/0.6745;
      } else {
	scale[j] = input_scale[j];
      }
      for (i=0; i < y_rows; i++){ 
	if (fabs(scale[j]) < 1e-10){
	  break;
	}
	wts[j*y_rows + i] = PsiFn(resids[j*y_rows + i]/scale[j],psi_k,0);
      }
    }


    /* printf("%f\n",scale); */


    /* weighted least squares */
    
    memset(xtwx,0,(y_cols)*(y_cols)*sizeof(double));


    colonly_XTWX(y_rows,y_cols,wts,xtwx);
    colonly_XTWXinv(y_rows, y_cols,xtwx);
    colonly_XTWY(y_rows, y_cols, wts,y, xtwy);

    
    for (i=0;i < y_cols; i++){
      out_beta[i] = 0.0;
       for (j=0;j < y_cols; j++){
    	 out_beta[i] += xtwx[j*y_cols + i]*xtwy[j];
       }
    }

    /* residuals */
    
    for (i=0; i < y_rows; i++){
      for (j=0; j < y_cols; j++){
	resids[j*y_rows +i] = y[j*y_rows + i] - probe_effects[i] - (out_beta[j]); 
      }
    }

    /*check convergence  based on residuals */
    
    conv = irls_delta(old_resids,resids, rows);
    
    if (conv < acc){
      /*    printf("Converged \n");*/
      break; 

    }
  }

  for (j = 0; j < y_cols; j++){
    if (input_scale[j] < 0.0){
      scale[j] = med_abs(&resids[j*y_rows],y_rows)/0.6745;
    } else {
      scale[j] = input_scale[j];
    }
  }

  R_Free(xtwx);
  R_Free(xtwy);
  R_Free(old_resids);
  R_Free(rowmeans);

  for (j = 0; j < y_cols; j++){
    input_scale[j] = scale[j];
  }
  R_Free(scale);
}


void rlm_fit_anova_given_probe_effects(double *y, int y_rows, int y_cols, double *probe_effects, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){

  double *scale=R_Calloc((y_cols),double);
  int j;

  for (j=0; j < y_cols; j++){
    scale[j] = -1.0;
  }
  rlm_fit_anova_given_probe_effects_engine(y, y_rows, y_cols, scale, probe_effects, out_beta, out_resids, out_weights, PsiFn, psi_k, max_iter, initialized);

  R_Free(scale);
}

void rlm_fit_anova_given_probe_effects_scale(double *y, int y_rows, int y_cols, double *input_scale, double *probe_effects, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){


  rlm_fit_anova_given_probe_effects_engine(y, y_rows, y_cols, input_scale, probe_effects, out_beta, out_resids, out_weights, PsiFn, psi_k, max_iter, initialized);


}



void rlm_fit_anova_given_probe_effects_R(double *y, int *rows, int *cols, double *probe_effects,double *out_beta, double *out_resids, double *out_weights, int *its){

  rlm_fit_anova_given_probe_effects(y, *rows, *cols, probe_effects,out_beta, out_resids,out_weights,psi_huber,1.345, *its,0);
}


/* Internal testing code (not an example that should ever be run)

   row.effects <- c(4,3,2,1,0,-1,-2,-3,-4)
   chip.effects <- c(10,11,12,10.5,11,9.5)

   y <- outer(row.effects,chip.effects,"+") + rnorm(54,sd=0.01)


   .C("rlm_fit_anova_given_probe_effects_R",as.double(y),as.integer(9),as.integer(6),as.double(row.effects),double(6),double(54),double(54),as.integer(20))



 */




void rlm_compute_se_anova_given_probe_effects(double *Y, int y_rows,int y_cols, double *probe_effects,double *beta, double *resids,double *weights,double *se_estimates, double *varcov, double *residSE, int method,double (* PsiFn)(double, double, int), double psi_k){
  
  int i,j; /* counter/indexing variables */
  double k1 = psi_k;   /*  was 1.345; */
  double sumpsi2=0.0;  /* sum of psi(r_i)^2 */
  /*  double sumpsi=0.0; */
  double sumderivpsi=0.0; /* sum of psi'(r_i) */
  double Kappa=0.0;      /* A correction factor */
  double scale=0.0;
  int n = y_rows*y_cols;
  int p = y_cols;
  double *XTX = R_Calloc(p*p,double);
  double *W = R_Calloc(p*p,double);
  double *work = R_Calloc(p*p,double);
  double RMSEw = 0.0;
  double vs=0.0,m,varderivpsi=0.0; 
  double *W_tmp=R_Calloc(n,double);

  /*
  ** The previous multi-chip code 
  
  if (method == 4){
    for (i=0; i < n; i++){
      RMSEw+= weights[i]*resids[i]*resids[i];
    }
    
    RMSEw = sqrt(RMSEw/(double)(n-p));

    residSE[0] =  RMSEw;


    colonly_XTWX(y_rows,y_cols,weights,XTX);
    if (y_rows > 1){
      colonly_XTWXinv(y_rows, y_cols,XTX);
    } else {
      for (i=0; i < p; i++){
	XTX[i*p + i] = 1.0/XTX[i*p + i];
      }
    }
    
    for (i =0; i < p; i++){
      se_estimates[i] = RMSEw*sqrt(XTX[i*p + i]);
    }
    
    
    if (varcov != NULL)
      for (i = 0; i < p; i++)
	for (j = i; j < p; j++)
	  varcov[j*p + i] =  RMSEw*RMSEw*XTX[j*p + i];
    

	  }
  */

  /* the new single chip code */
  colonly_XTWX(y_rows,y_cols,weights,XTX);
  if (y_rows > 1){
    colonly_XTWXinv(y_rows, y_cols,XTX);
  } else {
    for (i=0; i < p; i++){
      XTX[i*p + i] = 1.0/XTX[i*p + i];
    }
  }
  
  for (j=0; j < y_cols; j++){
    RMSEw = 0.0;
    for (i=0; i < y_rows; i++){
      RMSEw+= weights[j*y_rows + i]*resids[j*y_rows + i]*resids[j*y_rows + i];
    }
    RMSEw = sqrt(RMSEw/(double)(y_rows-1));

    se_estimates[j] = RMSEw*sqrt(XTX[j*p + j]);

  }


  

  R_Free(W_tmp);
  R_Free(work);
  R_Free(XTX);
  R_Free(W);

}



void rlm_wfit_anova_given_probe_effects_engine(double *y, int y_rows, int y_cols, double *input_scale, double *probe_effects, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){

  int i,j,iter;
  /* double tol = 1e-7; */
  double acc = 1e-4;
  double *scale  =R_Calloc((y_cols),double);
  double conv;
  double endprobe;

  double *wts = out_weights; 

  double *resids = out_resids; 
  double *old_resids = R_Calloc(y_rows*y_cols,double);
  
  double *rowmeans = R_Calloc(y_rows,double);

  double *xtwx = R_Calloc((y_cols)*(y_cols),double);
  double *xtwy = R_Calloc((y_cols),double);

  double sumweights, rows;
  
  rows = y_rows*y_cols;
  
  if (!initialized){
    
    /* intially use equal weights */
    for (i=0; i < rows; i++){
      wts[i] =  w[i]*1.0;
    }
  }

  /* starting matrix */
  
  for (i=0; i < y_rows; i++){
    for (j=0; j < y_cols; j++){
      resids[j*y_rows + i] = y[j*y_rows + i] - probe_effects[i];
    }
  }
  
  /* sweep columns (ie chip effects) */

  for (j=0; j < y_cols; j++){
    out_beta[j] = 0.0;
    sumweights = 0.0;
    for (i=0; i < y_rows; i++){
      out_beta[j] += wts[j*y_rows + i]* resids[j*y_rows + i];
      sumweights +=  wts[j*y_rows + i];
    }
    out_beta[j]/=sumweights;
    for (i=0; i < y_rows; i++){
      resids[j*y_rows + i] = resids[j*y_rows + i] -  out_beta[j];
    }
  }
  
  for (iter = 0; iter < max_iter; iter++){
    
    /* The new single-chip code */

    for (i =0; i < rows; i++){
      old_resids[i] = resids[i];
    }


    for (j = 0; j < y_cols; j++){ 
      if (input_scale[j] < 0.0){
	scale[j] = med_abs(&resids[j*y_rows],y_rows)/0.6745;
      } else {
	scale[j] = input_scale[j];
      }
   
      for (i=0; i < y_rows; i++){ 
	if (fabs(scale[j]) < 1e-10){
	  break;
	}
	wts[j*y_rows + i] = w[j*y_rows + i]*PsiFn(resids[j*y_rows + i]/scale[j],psi_k,0);
      }
    }


    /* printf("%f\n",scale); */


    /* weighted least squares */
    
    memset(xtwx,0,(y_cols)*(y_cols)*sizeof(double));


    colonly_XTWX(y_rows,y_cols,wts,xtwx);
    colonly_XTWXinv(y_rows, y_cols,xtwx);
    colonly_XTWY(y_rows, y_cols, wts,y, xtwy);

    
    for (i=0;i < y_cols; i++){
      out_beta[i] = 0.0;
       for (j=0;j < y_cols; j++){
    	 out_beta[i] += xtwx[j*y_cols + i]*xtwy[j];
       }
    }

    /* residuals */
    
    for (i=0; i < y_rows; i++){
      for (j=0; j < y_cols; j++){
	resids[j*y_rows +i] = y[j*y_rows + i] - probe_effects[i] - (out_beta[j]); 
      }
    }

    /*check convergence  based on residuals */
    
    conv = irls_delta(old_resids,resids, rows);
    
    if (conv < acc){
      /*    printf("Converged \n");*/
      break; 

    }
  }
  
  for (j = 0; j < y_cols; j++){
    if (input_scale[j] < 0.0){
      scale[j] = med_abs(&resids[j*y_rows],y_rows)/0.6745;
    } else {
      scale[j] = input_scale[j];
    }
  }
  R_Free(xtwx);
  R_Free(xtwy);
  R_Free(old_resids);
  R_Free(rowmeans);

  for (j = 0; j < y_cols; j++){
    input_scale[j] = scale[j];
  }
  R_Free(scale);
}



void rlm_wfit_anova_given_probe_effects(double *y, int y_rows, int y_cols, double *probe_effects, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){
  double *scale=R_Calloc((y_cols),double);
  int j;

  for (j=0; j < y_cols; j++){
    scale[j] = -1.0;
  }

  rlm_wfit_anova_given_probe_effects_engine(y, y_rows, y_cols, scale, probe_effects, w, out_beta, out_resids, out_weights, PsiFn, psi_k, max_iter, initialized);

  R_Free(scale);
}



void rlm_wfit_anova_given_probe_effects_scale(double *y, int y_rows, int y_cols, double *scale, double *probe_effects, double *w, double *out_beta, double *out_resids, double *out_weights,double (* PsiFn)(double, double, int), double psi_k,int max_iter, int initialized){
  rlm_wfit_anova_given_probe_effects_engine(y, y_rows, y_cols, scale, probe_effects, w, out_beta, out_resids, out_weights, PsiFn, psi_k, max_iter, initialized);
}
