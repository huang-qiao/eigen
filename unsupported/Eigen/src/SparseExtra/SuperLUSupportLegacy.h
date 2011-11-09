// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2008-2009 Gael Guennebaud <gael.guennebaud@inria.fr>
//
// Eigen is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 3 of the License, or (at your option) any later version.
//
// Alternatively, you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// Eigen is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License or the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License and a copy of the GNU General Public License along with
// Eigen. If not, see <http://www.gnu.org/licenses/>.

#ifndef EIGEN_SUPERLUSUPPORT_LEGACY_H
#define EIGEN_SUPERLUSUPPORT_LEGACY_H

/** \deprecated use class BiCGSTAB, class SuperLU, or class UmfPackLU */
template<typename MatrixType>
class SparseLU<MatrixType,SuperLULegacy> : public SparseLU<MatrixType>
{
  protected:
    typedef SparseLU<MatrixType> Base;
    typedef typename Base::Scalar Scalar;
    typedef typename Base::RealScalar RealScalar;
    typedef Matrix<Scalar,Dynamic,1> Vector;
    typedef Matrix<int, 1, MatrixType::ColsAtCompileTime> IntRowVectorType;
    typedef Matrix<int, MatrixType::RowsAtCompileTime, 1> IntColVectorType;
    typedef SparseMatrix<Scalar,Lower|UnitDiag> LMatrixType;
    typedef SparseMatrix<Scalar,Upper> UMatrixType;
    using Base::m_flags;
    using Base::m_status;

  public:

    /** \deprecated the entire class is deprecated */
  EIGEN_DEPRECATED SparseLU(int flags = NaturalOrdering)
      : Base(flags)
    {
    }

    /** \deprecated the entire class is deprecated */
    EIGEN_DEPRECATED SparseLU(const MatrixType& matrix, int flags = NaturalOrdering)
      : Base(flags)
    {
      compute(matrix);
    }

    ~SparseLU()
    {
      Destroy_SuperNode_Matrix(&m_sluL);
      Destroy_CompCol_Matrix(&m_sluU);
    }

    inline const LMatrixType& matrixL() const
    {
      if (m_extractedDataAreDirty) extractData();
      return m_l;
    }

    inline const UMatrixType& matrixU() const
    {
      if (m_extractedDataAreDirty) extractData();
      return m_u;
    }

    inline const IntColVectorType& permutationP() const
    {
      if (m_extractedDataAreDirty) extractData();
      return m_p;
    }

    inline const IntRowVectorType& permutationQ() const
    {
      if (m_extractedDataAreDirty) extractData();
      return m_q;
    }

    Scalar determinant() const;

    template<typename BDerived, typename XDerived>
    bool solve(const MatrixBase<BDerived> &b, MatrixBase<XDerived>* x, const int transposed = SvNoTrans) const;

    void compute(const MatrixType& matrix);

  protected:

    void extractData() const;

  protected:
    // cached data to reduce reallocation, etc.
    mutable LMatrixType m_l;
    mutable UMatrixType m_u;
    mutable IntColVectorType m_p;
    mutable IntRowVectorType m_q;

    mutable SparseMatrix<Scalar> m_matrix;
    mutable SluMatrix m_sluA;
    mutable SuperMatrix m_sluL, m_sluU;
    mutable SluMatrix m_sluB, m_sluX;
    mutable SuperLUStat_t m_sluStat;
    mutable superlu_options_t m_sluOptions;
    mutable std::vector<int> m_sluEtree;
    mutable std::vector<RealScalar> m_sluRscale, m_sluCscale;
    mutable std::vector<RealScalar> m_sluFerr, m_sluBerr;
    mutable char m_sluEqued;
    mutable bool m_extractedDataAreDirty;
};

template<typename MatrixType>
void SparseLU<MatrixType,SuperLULegacy>::compute(const MatrixType& a)
{
  const int size = a.rows();
  m_matrix = a;

  set_default_options(&m_sluOptions);
  m_sluOptions.ColPerm = NATURAL;
  m_sluOptions.PrintStat = NO;
  m_sluOptions.ConditionNumber = NO;
  m_sluOptions.Trans = NOTRANS;
  // m_sluOptions.Equil = NO;

  switch (Base::orderingMethod())
  {
      case NaturalOrdering          : m_sluOptions.ColPerm = NATURAL; break;
      case MinimumDegree_AT_PLUS_A  : m_sluOptions.ColPerm = MMD_AT_PLUS_A; break;
      case MinimumDegree_ATA        : m_sluOptions.ColPerm = MMD_ATA; break;
      case ColApproxMinimumDegree   : m_sluOptions.ColPerm = COLAMD; break;
      default:
        //std::cerr << "Eigen: ordering method \"" << Base::orderingMethod() << "\" not supported by the SuperLU backend\n";
        m_sluOptions.ColPerm = NATURAL;
  };

  m_sluA = internal::asSluMatrix(m_matrix);
  memset(&m_sluL,0,sizeof m_sluL);
  memset(&m_sluU,0,sizeof m_sluU);
  m_sluEqued = 'N';
  int info = 0;

  m_p.resize(size);
  m_q.resize(size);
  m_sluRscale.resize(size);
  m_sluCscale.resize(size);
  m_sluEtree.resize(size);

  RealScalar recip_pivot_gross, rcond;
  RealScalar ferr, berr;

  // set empty B and X
  m_sluB.setStorageType(SLU_DN);
  m_sluB.setScalarType<Scalar>();
  m_sluB.Mtype = SLU_GE;
  m_sluB.storage.values = 0;
  m_sluB.nrow = m_sluB.ncol = 0;
  m_sluB.storage.lda = size;
  m_sluX = m_sluB;

  StatInit(&m_sluStat);
  if (m_flags&IncompleteFactorization)
  {
    #ifdef EIGEN_SUPERLU_HAS_ILU
    ilu_set_default_options(&m_sluOptions);

    // no attempt to preserve column sum
    m_sluOptions.ILU_MILU = SILU;

    // only basic ILU(k) support -- no direct control over memory consumption
    // better to use ILU_DropRule = DROP_BASIC | DROP_AREA
    // and set ILU_FillFactor to max memory growth
    m_sluOptions.ILU_DropRule = DROP_BASIC;
    m_sluOptions.ILU_DropTol = Base::m_precision;

    SuperLU_gsisx(&m_sluOptions, &m_sluA, m_q.data(), m_p.data(), &m_sluEtree[0],
      &m_sluEqued, &m_sluRscale[0], &m_sluCscale[0],
      &m_sluL, &m_sluU,
      NULL, 0,
      &m_sluB, &m_sluX,
      &recip_pivot_gross, &rcond,
      &m_sluStat, &info, Scalar());
    #else
    //std::cerr << "Incomplete factorization is only available in SuperLU v4\n";
    Base::m_succeeded = false;
    return;
    #endif
  }
  else
  {
    SuperLU_gssvx(&m_sluOptions, &m_sluA, m_q.data(), m_p.data(), &m_sluEtree[0],
      &m_sluEqued, &m_sluRscale[0], &m_sluCscale[0],
      &m_sluL, &m_sluU,
      NULL, 0,
      &m_sluB, &m_sluX,
      &recip_pivot_gross, &rcond,
      &ferr, &berr,
      &m_sluStat, &info, Scalar());
  }
  StatFree(&m_sluStat);

  m_extractedDataAreDirty = true;

  // FIXME how to better check for errors ???
  Base::m_succeeded = (info == 0);
}

template<typename MatrixType>
template<typename BDerived,typename XDerived>
bool SparseLU<MatrixType,SuperLULegacy>::solve(const MatrixBase<BDerived> &b,
                        MatrixBase<XDerived> *x, const int transposed) const
{
  const int size = m_matrix.rows();
  const int rhsCols = b.cols();
  eigen_assert(size==b.rows());

  switch (transposed) {
      case SvNoTrans    :  m_sluOptions.Trans = NOTRANS; break;
      case SvTranspose  :  m_sluOptions.Trans = TRANS;   break;
      case SvAdjoint    :  m_sluOptions.Trans = CONJ;    break;
      default:
        //std::cerr << "Eigen: transposition  option \"" << transposed << "\" not supported by the SuperLU backend\n";
        m_sluOptions.Trans = NOTRANS;
  }

  m_sluOptions.Fact = FACTORED;
  m_sluOptions.IterRefine = NOREFINE;

  m_sluFerr.resize(rhsCols);
  m_sluBerr.resize(rhsCols);
  m_sluB = SluMatrix::Map(b.const_cast_derived());
  m_sluX = SluMatrix::Map(x->derived());
  
  typename BDerived::PlainObject b_cpy;
  if(m_sluEqued!='N')
  {
    b_cpy = b;
    m_sluB = SluMatrix::Map(b_cpy.const_cast_derived());  
  }

  StatInit(&m_sluStat);
  int info = 0;
  RealScalar recip_pivot_gross, rcond;

  if (m_flags&IncompleteFactorization)
  {
    #ifdef EIGEN_SUPERLU_HAS_ILU
    SuperLU_gsisx(&m_sluOptions, &m_sluA, m_q.data(), m_p.data(), &m_sluEtree[0],
      &m_sluEqued, &m_sluRscale[0], &m_sluCscale[0],
      &m_sluL, &m_sluU,
      NULL, 0,
      &m_sluB, &m_sluX,
      &recip_pivot_gross, &rcond,
      &m_sluStat, &info, Scalar());
    #else
    //std::cerr << "Incomplete factorization is only available in SuperLU v4\n";
    return false;
    #endif
  }
  else
  {
    SuperLU_gssvx(
      &m_sluOptions, &m_sluA,
      m_q.data(), m_p.data(),
      &m_sluEtree[0], &m_sluEqued,
      &m_sluRscale[0], &m_sluCscale[0],
      &m_sluL, &m_sluU,
      NULL, 0,
      &m_sluB, &m_sluX,
      &recip_pivot_gross, &rcond,
      &m_sluFerr[0], &m_sluBerr[0],
      &m_sluStat, &info, Scalar());
  }
  StatFree(&m_sluStat);

  // reset to previous state
  m_sluOptions.Trans = NOTRANS;
  return info==0;
}

//
// the code of this extractData() function has been adapted from the SuperLU's Matlab support code,
//
//  Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
//
//  THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY
//  EXPRESSED OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
//
template<typename MatrixType>
void SparseLU<MatrixType,SuperLULegacy>::extractData() const
{
  if (m_extractedDataAreDirty)
  {
    int         upper;
    int         fsupc, istart, nsupr;
    int         lastl = 0, lastu = 0;
    SCformat    *Lstore = static_cast<SCformat*>(m_sluL.Store);
    NCformat    *Ustore = static_cast<NCformat*>(m_sluU.Store);
    Scalar      *SNptr;

    const int size = m_matrix.rows();
    m_l.resize(size,size);
    m_l.resizeNonZeros(Lstore->nnz);
    m_u.resize(size,size);
    m_u.resizeNonZeros(Ustore->nnz);

    int* Lcol = m_l._outerIndexPtr();
    int* Lrow = m_l._innerIndexPtr();
    Scalar* Lval = m_l._valuePtr();

    int* Ucol = m_u._outerIndexPtr();
    int* Urow = m_u._innerIndexPtr();
    Scalar* Uval = m_u._valuePtr();

    Ucol[0] = 0;
    Ucol[0] = 0;

    /* for each supernode */
    for (int k = 0; k <= Lstore->nsuper; ++k)
    {
      fsupc   = L_FST_SUPC(k);
      istart  = L_SUB_START(fsupc);
      nsupr   = L_SUB_START(fsupc+1) - istart;
      upper   = 1;

      /* for each column in the supernode */
      for (int j = fsupc; j < L_FST_SUPC(k+1); ++j)
      {
        SNptr = &((Scalar*)Lstore->nzval)[L_NZ_START(j)];

        /* Extract U */
        for (int i = U_NZ_START(j); i < U_NZ_START(j+1); ++i)
        {
          Uval[lastu] = ((Scalar*)Ustore->nzval)[i];
          /* Matlab doesn't like explicit zero. */
          if (Uval[lastu] != 0.0)
            Urow[lastu++] = U_SUB(i);
        }
        for (int i = 0; i < upper; ++i)
        {
          /* upper triangle in the supernode */
          Uval[lastu] = SNptr[i];
          /* Matlab doesn't like explicit zero. */
          if (Uval[lastu] != 0.0)
            Urow[lastu++] = L_SUB(istart+i);
        }
        Ucol[j+1] = lastu;

        /* Extract L */
        Lval[lastl] = 1.0; /* unit diagonal */
        Lrow[lastl++] = L_SUB(istart + upper - 1);
        for (int i = upper; i < nsupr; ++i)
        {
          Lval[lastl] = SNptr[i];
          /* Matlab doesn't like explicit zero. */
          if (Lval[lastl] != 0.0)
            Lrow[lastl++] = L_SUB(istart+i);
        }
        Lcol[j+1] = lastl;

        ++upper;
      } /* for j ... */

    } /* for k ... */

    // squeeze the matrices :
    m_l.resizeNonZeros(lastl);
    m_u.resizeNonZeros(lastu);

    m_extractedDataAreDirty = false;
  }
}

template<typename MatrixType>
typename SparseLU<MatrixType,SuperLULegacy>::Scalar SparseLU<MatrixType,SuperLULegacy>::determinant() const
{
  assert((!NumTraits<Scalar>::IsComplex) && "This function is not implemented for complex yet");
  if (m_extractedDataAreDirty)
    extractData();

  // TODO this code could be moved to the default/base backend
  // FIXME perhaps we have to take into account the scale factors m_sluRscale and m_sluCscale ???
  Scalar det = Scalar(1);
  for (int j=0; j<m_u.cols(); ++j)
  {
    if (m_u._outerIndexPtr()[j+1]-m_u._outerIndexPtr()[j] > 0)
    {
      int lastId = m_u._outerIndexPtr()[j+1]-1;
      eigen_assert(m_u._innerIndexPtr()[lastId]<=j);
      if (m_u._innerIndexPtr()[lastId]==j)
      {
        det *= m_u._valuePtr()[lastId];
      }
    }
//       std::cout << m_sluRscale[j] << " " << m_sluCscale[j] << "   \n";
  }
  return det;
}

#endif // EIGEN_SUPERLUSUPPORT_LEGACY_H