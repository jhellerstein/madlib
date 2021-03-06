/* ----------------------------------------------------------------------- *//**
 *
 * @file logistic.cpp
 *
 * @brief Logistic-Regression functions
 *
 * We implement the conjugate-gradient method and the iteratively-reweighted-
 * least-squares method.
 *
 *//* ----------------------------------------------------------------------- */

#include <modules/regress/logistic.hpp>
#include <utils/Reference.hpp>

// Floating-point classification functions are in C99 and TR1, but not in the
// official C++ Standard (before C++0x). We therefore use the Boost implementation
#include <boost/math/special_functions/fpclassify.hpp>

// z values are normally distributed
#include <boost/math/distributions/normal.hpp>

// The squared z values of regression coefficients are approximately chi-squared
// distributed.
#include <boost/math/distributions/chi_squared.hpp>


// Import names from Armadillo
using arma::mat;
using arma::trans;
using arma::colvec;
using arma::as_scalar;


namespace madlib {

using utils::Reference;

namespace modules {

namespace regress {

// Local functions
AnyValue stateToResult(AbstractDBInterface &db,
    const DoubleCol &inCoef,
    const double &inLogLikelihood,
    const mat &inInverse_of_X_transp_AX);

/**
 * @brief Inter- and intra-iteration state for conjugate-gradient method for
 *        logistic regression
 *
 * TransitionState encapsualtes the transition state during the
 * logistic-regression aggregate function. To the database, the state is
 * exposed as a single DOUBLE PRECISION array, to the C++ code it is a proper
 * object containing scalars and vectors.
 *
 * Note: We assume that the DOUBLE PRECISION array is initialized by the
 * database with length at least 5, and all elemenets are 0.
 *
 * @internal Array layout (iteration refers to one aggregate-function call):
 * Inter-iteration components (updated in final function):
 * - 0: iteration (current iteration)
 * - 1: widthOfX (number of coefficients)
 * - 2: coef (vector of coefficients)
 * - 2 + widthOfX: dir (direction)
 * - 2 + 2 * widthOfX: grad (gradient)
 * - 2 + 3 * widthOfX: beta (scale factor)
 *
 * Intra-iteration components (updated in transition step):
 * - 3 + 3 * widthOfX: numRows (number of rows already processed in this iteration)
 * - 4 + 3 * widthOfX: gradNew (intermediate value for gradient)
 * - 4 + 4 * widthOfX: X_transp_AX (X^T A X)
 * - 4 + widthOfX * widthOfX + 4 * widthOfX: logLikelihood ( ln(l(c)) )
// * - 5 + widthOfX * widthOfX + 4 * widthOfX: dTHd (intermediate value for d^T * H * d)
 */
class LogisticRegressionCG::State {
public:
    State(AnyValue inArg)
        : mStorage(inArg.copyIfImmutable()),
          iteration(&mStorage[0]),
          widthOfX(&mStorage[1]),
          coef(TransparentHandle::create(&mStorage[2]),
               widthOfX),
          dir(TransparentHandle::create(&mStorage[2 + widthOfX]),
              widthOfX),
          grad(TransparentHandle::create(&mStorage[2 + 2 * widthOfX]),
              widthOfX),
          beta(&mStorage[2 + 3 * widthOfX]),
          
          numRows(&mStorage[3 + 3 * widthOfX]),
          gradNew(TransparentHandle::create(&mStorage[4 + 3 * widthOfX]),
                  widthOfX),
          X_transp_AX(TransparentHandle::create(&mStorage[4 + 4 * widthOfX]),
              widthOfX, widthOfX),
          logLikelihood(&mStorage[4 + widthOfX * widthOfX + 4 * widthOfX])
        { }
    
    /**
     * We define this function so that we can use State in the
     * argument list and as a return type.
     */
    inline operator AnyValue() const {
        return mStorage;
    }
    
    /**
     * @brief Initialize the conjugate-gradient state.
     * 
     * This function is only called for the first iteration, for the first row.
     */
    inline void initialize(AllocatorSPtr inAllocator,
        const uint16_t inWidthOfX) {
        
        mStorage.rebind(inAllocator, boost::extents[ arraySize(inWidthOfX) ]);
        iteration.rebind(&mStorage[0]) = 0;
        widthOfX.rebind(&mStorage[1]) = inWidthOfX;
        coef.rebind(TransparentHandle::create(&mStorage[2]),
                    widthOfX).zeros();
        dir.rebind(TransparentHandle::create(&mStorage[2 + widthOfX]),
                   widthOfX).zeros();
        grad.rebind(TransparentHandle::create(&mStorage[2 + 2 * widthOfX]),
                    widthOfX).zeros();
        beta.rebind(&mStorage[2 + 3 * widthOfX]) = 0;

        numRows.rebind(&mStorage[3 + 3 * widthOfX]);
        gradNew.rebind(TransparentHandle::create(&mStorage[4 + 3 * widthOfX]),
                       widthOfX);
        X_transp_AX.rebind(TransparentHandle::create(&mStorage[4 + 4 * widthOfX]),
              widthOfX, widthOfX);
        logLikelihood.rebind(&mStorage[4 + widthOfX * widthOfX + 4 * widthOfX]);
        reset();
    }
    
    /**
     * @brief We need to support assigning the previous state
     */
    State &operator=(const State &inOtherState) {
        mStorage = inOtherState.mStorage;
        return *this;
    }
    
    /**
     * @brief Merge with another State object by copying the intra-iteration fields
     */
    State &operator+=(const State &inOtherState) {
        if (mStorage.size() != inOtherState.mStorage.size() ||
            widthOfX != inOtherState.widthOfX)
            throw std::logic_error("Internal error: Incompatible transition states");
        
        numRows += inOtherState.numRows;
        gradNew += inOtherState.gradNew;
        X_transp_AX += inOtherState.X_transp_AX;
        logLikelihood += inOtherState.logLikelihood;
        return *this;
    }
    
    /**
     * @brief Reset the inter-iteration fields.
     */
    inline void reset() {
        numRows = 0;
        X_transp_AX.zeros();
        gradNew.zeros();
        logLikelihood = 0;
    }

private:
    static inline uint32_t arraySize(const uint16_t inWidthOfX) {
        return 5 + inWidthOfX * inWidthOfX + 4 * inWidthOfX;
    }

    Array<double> mStorage;

public:
    Reference<double, uint32_t> iteration;
    Reference<double, uint16_t> widthOfX;
    DoubleCol coef;
    DoubleCol dir;
    DoubleCol grad;
    Reference<double> beta;
    
    Reference<double, uint64_t> numRows;
    DoubleCol gradNew;
    DoubleMat X_transp_AX;
    Reference<double> logLikelihood;
//    Reference<double> dTHd;
};

/**
 * @brief Logistic function
 */
static double sigma(double x) {
	return 1. / (1. + std::exp(-x));
}

/**
 * @brief Perform the logistic-regression transition step
 */
AnyValue LogisticRegressionCG::transition(AbstractDBInterface &db, AnyValue args) {
    AnyValue::iterator arg(args);
    
    // Initialize Arguments from SQL call
    State state = *arg++;
    double y = *arg++ ? 1. : -1.;
    DoubleRow_const x = *arg++;
    if (state.numRows == 0) {
        state.initialize(db.allocator(AbstractAllocator::kAggregate), x.n_elem);
        if (!arg->isNull()) {
            const State previousState = *arg;
            
            state = previousState;
            state.reset();
        }
    }
    
    // Now do the transition step
    state.numRows++;
	
    double xc = as_scalar( x * state.coef );
	double xd = as_scalar( x * state.dir );
    
    state.gradNew += sigma(-y * xc) * y * trans(x);

    // Note: sigma(-x) = 1 - sigma(x).
    // a_i = sigma(x_i c) sigma(-x_i c)
    double a = sigma(xc) * sigma(-xc);
    state.X_transp_AX += trans(x) * a * x;

    //          n
    //         --
    // l(c) = -\  log(1 + exp(-y_i * c^T x_i))
    //         /_
    //         i=1
    state.logLikelihood -= std::log( 1. + std::exp(-y * xc) );
    return state;
}

/**
 * @brief Perform the perliminary aggregation function: Merge transition states
 */
AnyValue LogisticRegressionCG::mergeStates(AbstractDBInterface &db, AnyValue args) {
    State stateLeft = args[0].copyIfImmutable();
    const State stateRight = args[1];

    // We first handle the trivial case where this function is called with one
    // of the states being the initial state
    if (stateLeft.numRows == 0)
        return stateRight;
    else if (stateRight.numRows == 0)
        return stateLeft;
    
    // Merge states together and return
    stateLeft += stateRight;
    return stateLeft;
}

/**
 * @brief Perform the logistic-regression final step
 */
AnyValue LogisticRegressionCG::final(AbstractDBInterface &db, AnyValue args) {
    // Argument from SQL call
    State state = args[0].copyIfImmutable();
    
    // Note: k = state.iteration
    if (state.iteration == 0) {
		// Iteration computes the gradient
	
		state.dir = state.gradNew;
		state.grad = state.gradNew;
	} else {
        // We use the Hestenes-Stiefel update formula:
        //
		//            g_k^T (g_k - g_{k-1})
		// beta_k = -------------------------
		//          d_{k-1}^T (g_k - g_{k-1})
        colvec gradNewMinusGrad = state.gradNew - state.grad;
        state.beta
            = dot(state.gradNew, gradNewMinusGrad)
            / dot(state.dir, gradNewMinusGrad);
        
        // Alternatively, we could use Polak-Ribière
        // state.beta
        //     = dot(state.gradNew, gradNewMinusGrad)
        //     / dot(state.grad, state.grad);
        
        // Or Fletcher–Reeves
        // state.beta
        //     = dot(state.gradNew, state.gradNew)
        //     / dot(state.grad, state.grad);
        
        // Do a direction restart (Powell restart)
        // Note: This is testing whether state.beta < 0 if state.beta were
        // assigned according to Polak-Ribière
        if (dot(state.gradNew, gradNewMinusGrad)
            / dot(state.grad, state.grad) < 0) state.beta = 0;
        
        // d_k = g_k - beta_k * d_{k-1}
        state.dir = state.gradNew - state.beta * state.dir;
		state.grad = state.gradNew;
	}

    // H_k = - X^T A_k X
    // where A_k = diag(a_1, ..., a_n) and a_i = sigma(x_i c_{k-1}) sigma(-x_i c_{k-1})
    //
    //             g_k^T d_k
    // alpha_k = -------------
    //           d_k^T H_k d_k
    //
    // c_k = c_{k-1} - alpha_k * d_k
    state.coef += dot(state.grad, state.dir) /
        as_scalar(trans(state.dir) * state.X_transp_AX * state.dir)
        * state.dir;

    state.iteration++;
    return state;
}

/**
 * @brief Return the difference in log-likelihood between two states
 */
AnyValue LogisticRegressionCG::distance(AbstractDBInterface &db, AnyValue args) {
    const State stateLeft = args[0];
    const State stateRight = args[1];

    return std::abs(stateLeft.logLikelihood - stateRight.logLikelihood);
}

/**
 * @brief Return the coefficients and diagnostic statistics of the state
 */
AnyValue LogisticRegressionCG::result(AbstractDBInterface &db, AnyValue args) {
    const State state = args[0];

    // Compute (X^T * A * X)^+
    mat inverse_of_X_transp_AX = pinv(state.X_transp_AX);
    
    return stateToResult(db, state.coef, state.logLikelihood,
        inverse_of_X_transp_AX);
}

/**
 * @brief Inter- and intra-iteration state for iteratively-reweighted-least-
 *        squares method for logistic regression
 *
 * TransitionState encapsualtes the transition state during the
 * logistic-regression aggregate function. To the database, the state is
 * exposed as a single DOUBLE PRECISION array, to the C++ code it is a proper
 * object containing scalars, a vector, and a matrix.
 *
 * Note: We assume that the DOUBLE PRECISION array is initialized by the
 * database with length at least 4, and all elemenets are 0.
 *
 * @internal Array layout (iteration refers to one aggregate-function call):
 * Inter-iteration components (updated in final function):
 * - 0: widthOfX (number of coefficients)
 * - 1: coef (vector of coefficients)
 *
 * Intra-iteration components (updated in transition step):
 * - 1 + widthOfX: numRows (number of rows already processed in this iteration)
 * - 2 + widthOfX: X_transp_Az (X^T A z)
 * - 2 + 2 * widthOfX: X_transp_AX (X^T A X)
 * - 2 + widthOfX^2 + 2 * widthOfX: logLikelihood ( ln(l(c)) )
 */
class LogisticRegressionIRLS::State {
public:
    State(AnyValue inArg)
        : mStorage(inArg.copyIfImmutable()),
          widthOfX(&mStorage[0]),
          coef(TransparentHandle::create(&mStorage[1]),
               widthOfX),
        
          numRows(&mStorage[1 + widthOfX]),
          X_transp_Az(TransparentHandle::create(&mStorage[2 + widthOfX]),
              widthOfX),
          X_transp_AX(TransparentHandle::create(&mStorage[2 + 2 * widthOfX]),
              widthOfX, widthOfX),
          logLikelihood(&mStorage[2 + widthOfX * widthOfX + 2 * widthOfX])
        { }
    
    /**
     * We define this function so that we can use State in the
     * argument list and as a return type.
     */
    inline operator AnyValue() const {
        return mStorage;
    }
    
    /**
     * @brief Initialize the conjugate-gradient state.
     * 
     * This function is only called for the first iteration, for the first row.
     */
    inline void initialize(AllocatorSPtr inAllocator,
        const uint16_t inWidthOfX) {
        
        mStorage.rebind(inAllocator, boost::extents[ arraySize(inWidthOfX) ]);
        widthOfX.rebind(&mStorage[0]) = inWidthOfX;
        coef.rebind(TransparentHandle::create(&mStorage[1]),
                    widthOfX).zeros();
        
        numRows.rebind(&mStorage[1 + widthOfX]);
        X_transp_Az.rebind(TransparentHandle::create(&mStorage[2 + widthOfX]),
                           widthOfX);
        X_transp_AX.rebind(TransparentHandle::create(&mStorage[2 + 2 * widthOfX]),
                           widthOfX, widthOfX);
        logLikelihood.rebind(&mStorage[2 + widthOfX * widthOfX + 2 * widthOfX]);
        reset();
    }
    
    /**
     * @brief We need to support assigning the previous state
     */
    State &operator=(const State &inOtherState) {
        mStorage = inOtherState.mStorage;
        return *this;
    }
    
    /**
     * @brief Merge with another State object by copying the intra-iteration fields
     */
    State &operator+=(const State &inOtherState) {
        if (mStorage.size() != inOtherState.mStorage.size() ||
            widthOfX != inOtherState.widthOfX)
            throw std::logic_error("Internal error: Incompatible transition states");
        
        numRows += inOtherState.numRows;
        X_transp_Az += inOtherState.X_transp_Az;
        X_transp_AX += inOtherState.X_transp_AX;
        logLikelihood += inOtherState.logLikelihood;
        return *this;
    }
    
    /**
     * @brief Reset the inter-iteration fields.
     */
    inline void reset() {
        numRows = 0;
        X_transp_Az.zeros();
        X_transp_AX.zeros();
        logLikelihood = 0;
    }
    
private:
    static inline uint32_t arraySize(const uint16_t inWidthOfX) {
        return 3 + inWidthOfX * inWidthOfX + 2 * inWidthOfX;
    }

    Array<double> mStorage;

public:
    Reference<double, uint16_t> widthOfX;
    DoubleCol coef;

    Reference<double, uint64_t> numRows;
    DoubleCol X_transp_Az;
    DoubleMat X_transp_AX;
    Reference<double> logLikelihood;
};

AnyValue LogisticRegressionIRLS::transition(AbstractDBInterface &db,
    AnyValue args) {
    AnyValue::iterator arg(args);
    
    // Initialize Arguments from SQL call
    State state = *arg++;
    double y = *arg++ ? 1. : -1.;
    DoubleRow_const x = *arg++;

    // See MADLIB-138. At least on certain platforms and with certain versions,
    // LAPACK will run into an infinite loop if pinv() is called for non-finite
    // matrices. We extend the check also to the dependent variables.
    if (!boost::math::isfinite(y))
        throw std::invalid_argument("Dependent variables are not finite.");
    else if (!x.is_finite())
        throw std::invalid_argument("Design matrix is not finite.");

    if (state.numRows == 0) {
        state.initialize(db.allocator(AbstractAllocator::kAggregate), x.n_elem);
        if (!arg->isNull()) {
            const State previousState = *arg;
            
            state = previousState;
            state.reset();
        }
    }
    
    // Now do the transition step
    state.numRows++;

    // xc = x_i c
    double xc = as_scalar( x * state.coef );
        
    // a_i = sigma(x_i c) sigma(-x_i c)
    double a = sigma(xc) * sigma(-xc);
    
    // Note: sigma(-x) = 1 - sigma(x).
    //
    //             sigma(-y_i x_i c) y_i
    // z = x_i c + ---------------------
    //                     a_i
    double z = xc + sigma(-y * xc) * y / a;

    state.X_transp_Az += trans(x) * a * z;
    state.X_transp_AX += trans(x) * a * x;
        
    // We use state.sumy to store the log likelihood.
    //          n
    //         --
    // l(c) = -\  ln(1 + exp(-y_i * c^T x_i))
    //         /_
    //         i=1
    state.logLikelihood -= std::log( 1. + std::exp(-y * xc) );
    return state;
}

/**
 * @brief Perform the perliminary aggregation function: Merge transition states
 */
AnyValue LogisticRegressionIRLS::mergeStates(AbstractDBInterface &db, AnyValue args) {
    State stateLeft = args[0].copyIfImmutable();
    const State stateRight = args[1];
    
    // We first handle the trivial case where this function is called with one
    // of the states being the initial state
    if (stateLeft.numRows == 0)
        return stateRight;
    else if (stateRight.numRows == 0)
        return stateLeft;
    
    // Merge states together and return
    stateLeft += stateRight;
    return stateLeft;
}

/**
 * @brief Perform the logistic-regression final step
 */
AnyValue LogisticRegressionIRLS::final(AbstractDBInterface &db, AnyValue args) {
    // Argument from SQL call
    State state = args[0].copyIfImmutable();

    // See MADLIB-138. At least on certain platforms and with certain versions,
    // LAPACK will run into an infinite loop if pinv() is called for non-finite
    // matrices. We extend the check also to the dependent variables.
    if (!state.X_transp_AX.is_finite() || !state.X_transp_Az.is_finite())
        throw std::invalid_argument("Design matrix is not finite.");
    
    state.coef = pinv(state.X_transp_AX) * state.X_transp_Az;
    
    return state;
}

/**
 * @brief Return the difference in log-likelihood between two states
 */
AnyValue LogisticRegressionIRLS::distance(AbstractDBInterface &db, AnyValue args) {
    const State stateLeft = args[0];
    const State stateRight = args[1];

    return std::abs(stateLeft.logLikelihood - stateRight.logLikelihood);
}

/**
 * @brief Return the coefficients and diagnostic statistics of the state
 */
AnyValue LogisticRegressionIRLS::result(AbstractDBInterface &db, AnyValue args) {
    const State state = args[0];

    // Compute (X^T * A * X)^+
    mat inverse_of_X_transp_AX = pinv(state.X_transp_AX);
    
    return stateToResult(db, state.coef, state.logLikelihood,
        inverse_of_X_transp_AX);
}

/**
 * @brief Compute the diagnostic statistics
 *
 * This function wraps the common parts of computing the results for both the
 * CG and the IRLS method.
 */
AnyValue stateToResult(AbstractDBInterface &db,
    const DoubleCol &inCoef,
    const double &inLogLikelihood,
    const mat &inInverse_of_X_transp_AX) {
    
    DoubleCol stdErr(db.allocator(), inCoef.n_elem);
    DoubleCol waldZStats(db.allocator(), inCoef.n_elem);
    DoubleCol waldPValues(db.allocator(), inCoef.n_elem);
    DoubleCol oddRatios(db.allocator(), inCoef.n_elem);
    for (unsigned int i = 0; i < inCoef.n_elem; i++) {
        stdErr(i) = std::sqrt(inInverse_of_X_transp_AX(i,i));
        waldZStats(i) = inCoef(i) / stdErr(i);
        waldPValues(i) = 2. *  ( boost::math::cdf(boost::math::normal(),
            -std::abs( waldZStats(i) )) );
        oddRatios(i) = std::exp( inCoef(i) );
    }

    // Return all coefficients, standard errors, etc. in a tuple
    AnyValueVector tuple;
    ConcreteRecord::iterator tupleElement(tuple);
    
    tupleElement++ = inCoef;
    tupleElement++ = inLogLikelihood;
    tupleElement++ = stdErr;
    tupleElement++ = waldZStats;
    tupleElement++ = waldPValues;
    tupleElement++ = oddRatios;
    
    return tuple;
}

} // namespace regress

} // namespace modules

} // namespace madlib
