#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layers/inner_product_layer.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/spgemm.hpp"

extern std::map<std::string, unsigned long long> total_conv_cycles;
extern std::map<std::string, double> total_conv_flops;
extern int total_files;

double get_cpu_freq();

namespace caffe {

template<typename Dtype>
InnerProductLayer<Dtype>::InnerProductLayer(const LayerParameter& param) :
    Layer<Dtype>(param),
    bottom_values_(NULL), bottom_j_(NULL), bottom_i_(NULL),
    top_values_(NULL), top_j_(NULL), top_i_(NULL),
    weight_values_(NULL), weight_j_(NULL), weight_i_(NULL),
    bottom_transposed_(NULL), B_temp_global_(NULL), C_temp_global_(NULL),
    weight_values_blocked_(NULL), weight_j_blocked_(NULL), weight_i_blocked_(NULL)
{

}

template<typename Dtype>
InnerProductLayer<Dtype>::~InnerProductLayer()
{
  free(bottom_values_);
  free(bottom_j_);
  free(bottom_i_);
  free(bottom_transposed_);

  free(top_values_);
  free(top_j_);
  free(top_i_);

  free(weight_values_);
  free(weight_j_);
  free(weight_i_);

  free(B_temp_global_);
  free(C_temp_global_);

  free(weight_values_blocked_);
  free(weight_j_blocked_);
  free(weight_i_blocked_);
}

enum
{
  SPGEMM_CSR,
  SPGEMM_CSC,
  SPMDM_CSR,
  SPMDM_CSC,
  GEMM,
};

static int method = GEMM; //SPMDM_CSR;

template<>
void InnerProductLayer<double>::WeightAlign(){
  NOT_IMPLEMENTED;
}

static int col_block_size = 128;

template<>
void InnerProductLayer<float>::WeightAlign(){
	const LayerParameter& layerparam = this->layer_param();
	LOG(INFO)<<"layer\t"<<layerparam.name()<<"\t"<<"has sparsity of "<< this->blobs_[0]->GetSparsity() << " transpose " << transpose_;
//	this->blobs_[0]->WriteToNistMMIO(layerparam.name()+".weight");

	posix_memalign((void **)&weight_i_, 4096, sizeof(int)*(std::max(K_, N_) + 1));
	posix_memalign((void **)&weight_j_, 4096, sizeof(int)*K_*N_);
	posix_memalign((void **)&weight_values_, 4096, sizeof(float)*K_*N_);

	posix_memalign((void **)&B_temp_global_, 4096, sizeof(float)*omp_get_max_threads()*4096);
	posix_memalign((void **)&C_temp_global_, 4096, sizeof(float)*omp_get_max_threads()*4096);

  MKL_INT job[] = {
      0 /*dense->CSR*/,
      0 /*0-based indexing in dense matrix */,
      0 /*1-based CSR*/,
      2 /* whole matrix*/,
      K_*N_, /* nzmax */
      1 /* generate a, i, and j */
  };
  MKL_INT info;
  if (SPMDM_CSR == method && !transpose_) {
    int m = transpose_ ? K_ : N_;
    int n = transpose_ ? N_ : K_;

    int ncolblocks = K_/col_block_size;
    posix_memalign((void **)&weight_i_blocked_, 4096, sizeof(int)*(N_*ncolblocks + 1));
    posix_memalign((void **)&weight_j_blocked_, 4096, sizeof(int)*K_*N_);
    posix_memalign((void **)&weight_values_blocked_, 4096, sizeof(float)*K_*N_);

    weight_i_blocked_[0] = 0;
    int nnz = 0;
    for (int cb = 0; cb < ncolblocks; ++cb) {
      for (int i = 0; i < N_; ++i) {
        for (int j = cb*col_block_size; j < (cb + 1)*col_block_size; ++j) {
          float v = this->blobs_[0]->mutable_cpu_data()[i*K_ + j];
          if (v != 0) {
            weight_j_blocked_[nnz] = M_*j;
            weight_values_blocked_[nnz] = v;
            ++nnz;
          }
        }
        weight_i_blocked_[cb*N_ + i + 1] = nnz;
      }
    }
  }
  else if (SPGEMM_CSR == method && transpose_ || SPGEMM_CSC == method && !transpose_) {
	  int m = transpose_ ? K_ : N_;
	  int n = transpose_ ? N_ : K_;
	  mkl_sdnscsr(job, &m, &n, this->blobs_[0]->mutable_cpu_data(), &n, weight_values_, weight_j_, weight_i_, &info);
	  if(info) {
	    LOG(FATAL)<<"The routine is interrupted processing the "<<
	        info<<"-th row "
	        <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
	  }
	}
	else if (SPGEMM_CSR == method && !transpose_ || SPGEMM_CSC == method && transpose_) {
	  int m = transpose_ ? K_ : N_;
	  int n = transpose_ ? N_ : K_;

    float *weight_transposed;
    posix_memalign((void **)&weight_transposed, 4096, sizeof(float)*K_*N_);
    mkl_somatcopy('R', 'T', m, n, 1, this->blobs_[0]->mutable_cpu_data(), n, weight_transposed, m);
    mkl_sdnscsr(job, &n, &m, weight_transposed, &m, weight_values_, weight_j_, weight_i_, &info);
    if(info) {
      LOG(FATAL)<<"The routine is interrupted processing the "<<
          info<<"-th row "
          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
    }
    free(weight_transposed);
	}

  posix_memalign((void **)&bottom_i_, 4096, sizeof(int)*(std::max(M_, K_) + 1));
  posix_memalign((void **)&bottom_j_, 4096, sizeof(int)*M_*K_);
  posix_memalign((void **)&bottom_values_, 4096, sizeof(float)*M_*K_);

  posix_memalign((void **)&top_i_, 4096, sizeof(int)*(std::max(M_, N_) + 1));
  posix_memalign((void **)&top_j_, 4096, sizeof(int)*M_*N_);
  posix_memalign((void **)&top_values_, 4096, sizeof(float)*M_*N_);

  posix_memalign((void **)&bottom_transposed_, 4096, sizeof(int)*M_*std::max(K_, N_));

	//disconnect connections
	if( layerparam.connectivity_mode() == caffe::LayerParameter_ConnectivityMode_DISCONNECTED_ELTWISE ){
		LOG(INFO)<<"all zero weights of "<<layerparam.name()<<" are frozen";
		this->blobs_[0]->Disconnect(Blob<float>::ELTWISE);
	}else if(layerparam.connectivity_mode() == caffe::LayerParameter_ConnectivityMode_DISCONNECTED_GRPWISE){
		LOG(INFO)<<"weights lying in all-zero groups of "<<layerparam.name()<<" are frozen";
		this->blobs_[0]->Disconnect(Blob<float>::GRPWISE);
	}
}

template <typename Dtype>
void InnerProductLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int num_output = this->layer_param_.inner_product_param().num_output();
  bias_term_ = this->layer_param_.inner_product_param().bias_term();
  transpose_ = this->layer_param_.inner_product_param().transpose();
    // if true, weight is in row-major, otherwise it's in col-major
  N_ = num_output;
  const int axis = bottom[0]->CanonicalAxisIndex(
      this->layer_param_.inner_product_param().axis());
  // Dimensions starting from "axis" are "flattened" into a single
  // length K_ vector. For example, if bottom[0]'s shape is (N, C, H, W),
  // and axis == 1, N inner products with dimension CHW are performed.
  K_ = bottom[0]->count(axis);
  // Check if we need to set up the weights
  if (this->blobs_.size() > 0) {
    LOG(INFO) << "Skipping parameter initialization";
  } else {
    if (bias_term_) {
      this->blobs_.resize(2);
    } else {
      this->blobs_.resize(1);
    }
    // Initialize the weights
    vector<int> weight_shape(2);
    if (transpose_) {
      weight_shape[0] = K_;
      weight_shape[1] = N_;
    } else {
      weight_shape[0] = N_;
      weight_shape[1] = K_;
    }
    this->blobs_[0].reset(new Blob<Dtype>(weight_shape));
    // fill the weights
    shared_ptr<Filler<Dtype> > weight_filler(GetFiller<Dtype>(
        this->layer_param_.inner_product_param().weight_filler()));
    weight_filler->Fill(this->blobs_[0].get());
    // If necessary, intiialize and fill the bias term
    if (bias_term_) {
      vector<int> bias_shape(1, N_);
      this->blobs_[1].reset(new Blob<Dtype>(bias_shape));
      shared_ptr<Filler<Dtype> > bias_filler(GetFiller<Dtype>(
          this->layer_param_.inner_product_param().bias_filler()));
      bias_filler->Fill(this->blobs_[1].get());
    }
  }  // parameter initialization
  this->param_propagate_down_.resize(this->blobs_.size(), true);
}

template <typename Dtype>
void InnerProductLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  // Figure out the dimensions
  const int axis = bottom[0]->CanonicalAxisIndex(
      this->layer_param_.inner_product_param().axis());
  const int new_K = bottom[0]->count(axis);
  CHECK_EQ(K_, new_K)
      << "Input size incompatible with inner product parameters.";
  // The first "axis" dimensions are independent inner products; the total
  // number of these is M_, the product over these dimensions.
  M_ = bottom[0]->count(0, axis);
  // The top shape will be the bottom shape with the flattened axes dropped,
  // and replaced by a single axis with dimension num_output (N_).
  vector<int> top_shape = bottom[0]->shape();
  top_shape.resize(axis + 1);
  top_shape[axis] = N_;
  top[0]->Reshape(top_shape);
  // Set up the bias multiplier
  if (bias_term_) {
    vector<int> bias_shape(1, M_);
    bias_multiplier_.Reshape(bias_shape);
    caffe_set(M_, Dtype(1), bias_multiplier_.mutable_cpu_data());
  }
}

template<>
void InnerProductLayer<double>::Forward_cpu(const vector<Blob<double>*>& bottom,
    const vector<Blob<double>*>& top) {
  NOT_IMPLEMENTED;
}

template<>
void InnerProductLayer<float>::Forward_cpu(const vector<Blob<float>*>& bottom,
    const vector<Blob<float>*>& top) {
  float* bottom_data = bottom[0]->mutable_cpu_data();
  float* top_data = top[0]->mutable_cpu_data();
  float* weight = this->blobs_[0]->mutable_cpu_data();

  bool PRINT_FEATURE_SPARSITY = false;
  if (PRINT_FEATURE_SPARSITY) {
    int cnt = 0;
    for (int i = 0; i < M_*K_; ++i) {
      if (bottom_data[i] == 0) ++cnt;
    }
    LOG(INFO) << this->layer_param_.name() << " M " << M_ << " K " << K_ << " N " << N_ << " sparsity " << (double)cnt/(M_*K_);
  }

  MKL_INT job[] = {
      0 /*dense->CSR*/,
      0 /*0-based indexing in dense matrix */,
      0 /*0-based CSR*/,
      2 /* whole matrix*/,
      M_*K_, /* nzmax */
      1 /* generate a, i, and j */
  };
  MKL_INT info;

  if (SPMDM_CSR == method) {
//    mkl_somatcopy('R', 'T', M_, K_, 1, bottom_data, K_, bottom_transposed_, M_);

    int ncolblocks = K_/col_block_size;
    double t = omp_get_wtime();
    csrmm(
        weight_values_blocked_, weight_j_blocked_, weight_i_blocked_,
        bottom_data,
        top_data,
        N_, M_, K_,
        this->blobs_[1]->cpu_data(),
        col_block_size);
    t = omp_get_wtime() - t;
    LOG(INFO) << "csrmm takes " << t << " effective GF/s " << 2.*K_*N_*M_/t/1e9 << " real GF/s " << 2.*weight_i_blocked_[ncolblocks*N_]*M_/t/1e9;

    memcpy(bottom_transposed_, top_data, sizeof(float)*M_*N_);
    mkl_somatcopy('R', 'T', N_, M_, 1, bottom_transposed_, M_, top_data, N_);

    std::string name(this->layer_param_.name());
    if (total_conv_cycles.find(name) == total_conv_cycles.end()) {
      total_conv_cycles[name] = 0;
      total_conv_flops[name] = 0;
    }
    total_conv_cycles[name] += t*get_cpu_freq();
    total_conv_flops[name] += 2.*M_*K_*N_;
    total_files += M_;
  }
  else if (SPGEMM_CSR == method) {
    float *A = layer2bottom["fc6"];

    CSR B = layer2weight["fc6"];
    CSR C = layer2weight["fc7"];

    float *B_bias = layer2bias["fc6"];
    float *C_bias = layer2bias["fc7"];

//    caffe_cpu_gemm<float>(CblasNoTrans, transpose_ ? CblasNoTrans : CblasTrans,
//        M_, N_, K_, (float)1.,
//        bottom_data, weight, (float)0., top_data);
//
//    if (bias_term_) {
//      // JSP: common path for AlexNet
//      caffe_cpu_gemm<float>(CblasNoTrans, CblasNoTrans, M_, N_, 1, (float)1.,
//          bias_multiplier_.cpu_data(),
//          this->blobs_[1]->cpu_data(), (float)1., top_data);
//    }
//    printf("%s %g\n", this->layer_param_.name().c_str(), top_data[123*N_ + 123]);

    int flops = csrmultd_fused_flops(
        A,
        B.values, B.colidx, B.rowptr,
        C.values, C.colidx, C.rowptr,
        weight_values_, weight_j_, weight_i_,
        B_bias, C_bias, this->blobs_[1]->cpu_data(),
        top_data,
        M_,
        B.m, B.n, K_, N_,
        B_temp_global_, C_temp_global_);

    int cnt = 0;
    for (int i = 0; i < B.m; ++i) {
      if (B_bias[i] <= 0) ++cnt;
    }
    LOG(INFO) << "B_bias sparsity " << (double)cnt/B.m;
    cnt = 0;
    for (int i = 0; i < K_; ++i) {
      if (C_bias[i] <= 0) ++cnt;
    }
    LOG(INFO) << "C_bias sparsity " << (double)cnt/K_;
    cnt = 0;
    for (int i = 0; i < N_; ++i) {
      if (this->blobs_[1]->cpu_data()[i] <= 0) ++cnt;
    }
    LOG(INFO) << "D_bias sparsity " << (double)cnt/N_;

    assert(C.n == K_);
    double t = omp_get_wtime();
    csrmultd_fused(
        A,
        B.values, B.colidx, B.rowptr,
        C.values, C.colidx, C.rowptr,
        weight_values_, weight_j_, weight_i_,
        B_bias, C_bias, this->blobs_[1]->cpu_data(),
        top_data,
        M_,
        B.m, B.n, K_, N_,
        B_temp_global_, C_temp_global_);
    t = omp_get_wtime() - t;

    LOG(INFO) << "fused_spgemm takes " << t << " GF/s= " << (double)flops/t/1e9 << " flop-sparsity " << 1 - (double)flops/(2.*(M_*B.m*B.n + M_*C.m*C.n + M_*K_*N_));

//    printf("E[123][123] = %g\n", top_data[123*N_ + 123]);

//    mkl_sdnscsr(job, &M_, &K_, bottom_data, &K_, bottom_values_, bottom_j_, bottom_i_, &info);
//    if(info) {
//      LOG(FATAL)<<"The routine is interrupted processing the "<<
//          info<<"-th row "
//          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
//    }
//
//    char transa = 'N';
//    MKL_INT request = 0; // output pre-allocated
//    MKL_INT sort = 0; // sort output
//    MKL_INT nzmax = M_*N_;
//    MKL_INT info;
//
//    mkl_scsrmultcsr(
//        &transa, &request, &sort, &M_, &K_, &N_,
//        bottom_values_, bottom_j_, bottom_i_,
//        weight_values_, weight_j_, weight_i_,
//        top_values_, top_j_, top_i_,
//        &nzmax, &info);
//    if(info) {
//      LOG(FATAL)<<"The routine is interrupted processing the "<<
//          info<<"-th row "
//          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
//    }
//
//    caffe_cpu_gemm<float>(CblasNoTrans, transpose_ ? CblasNoTrans : CblasTrans,
//        M_, N_, K_, (float)1.,
//        bottom_data, weight, (float)0., top_data);
//
////    memset(top_data, 0, sizeof(float)*M_*N_);
//    for (int i = 0; i < M_; ++i) {
//      for (int j = top_i_[i] - 1; j < top_i_[i + 1] - 1; ++j) {
//        float expected = top_data[i*N_ + top_j_[j] - 1];
//        float actual = top_values_[j];
//        float expected2 = 0;
//        for (int k = 0; k < K_; ++k) {
//          expected2 += bottom_data[i*K_ + k]*weight[k*N_ + top_j_[j] - 1];
//        }
//        if (fabs(expected - actual)/fabs(expected) > 0.1 && fabs(expected) > 1e-4 && fabs(actual) > 1e-4) {
//          LOG(FATAL) << "expected " << expected << " actual " << actual;
//        }
//      }
//    }
//
////    job[0] = 1; /* CSR->dense */
////    job[4] = M_*N_;
////    mkl_sdnscsr(job, &M_, &N_, top_data, &N_, top_values_, top_j_, top_i_, &info);
////    if(info) {
////      LOG(FATAL)<<"The routine is interrupted processing the "<<
////          info<<"-th row "
////          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
////    }
  }
  else if (SPGEMM_CSC == method) {
    mkl_somatcopy('R', 'T', M_, K_, 1, bottom_data, K_, bottom_transposed_, M_);
    mkl_sdnscsr(job, &K_, &M_, bottom_transposed_, &M_, bottom_values_, bottom_j_, bottom_i_, &info);
    if(info) {
      LOG(FATAL)<<"The routine is interrupted processing the "<<
          info<<"-th row "
          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
    }

    char transa = 'N';
    MKL_INT request = 0; // output pre-allocated
    MKL_INT sort = 0; // sort output
    MKL_INT nzmax = M_*N_;
    MKL_INT info;

    mkl_scsrmultcsr(
        &transa, &request, &sort, &N_, &K_, &M_,
        weight_values_, weight_j_, weight_i_,
        bottom_values_, bottom_j_, bottom_i_,
        top_values_, top_j_, top_i_,
        &nzmax, &info);
    if(info) {
      LOG(FATAL)<<"The routine is interrupted processing the "<<
          info<<"-th row "
          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
    }

    job[0] = 1; /* CSR->dense */
    job[4] = M_*N_;
    mkl_sdnscsr(job, &N_, &M_, bottom_transposed_, &M_, top_values_, top_j_, top_i_, &info);
    if(info) {
      LOG(FATAL)<<"The routine is interrupted processing the "<<
          info<<"-th row "
          <<"because there is no space in the arrays acsr and ja according to the value nzmax.";
    }
    mkl_somatcopy('R', 'T', N_, M_, 1, bottom_transposed_, M_, top_data, N_);
  }
  else {
    assert(GEMM == method);
    caffe_cpu_gemm<float>(CblasNoTrans, transpose_ ? CblasNoTrans : CblasTrans,
        M_, N_, K_, (float)1.,
        bottom_data, weight, (float)0., top_data);

    if (bias_term_) {
      // JSP: common path for AlexNet
      caffe_cpu_gemm<float>(CblasNoTrans, CblasNoTrans, M_, N_, 1, (float)1.,
          bias_multiplier_.cpu_data(),
          this->blobs_[1]->cpu_data(), (float)1., top_data);
    }
  }
}

template <typename Dtype>
void InnerProductLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  if (this->param_propagate_down_[0]) {
    const Dtype* top_diff = top[0]->cpu_diff();
    const Dtype* bottom_data = bottom[0]->cpu_data();
    // Gradient with respect to weight
    if (transpose_) {
      caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans,
          K_, N_, M_,
          (Dtype)1., bottom_data, top_diff,
          (Dtype)1., this->blobs_[0]->mutable_cpu_diff());
    } else {
      caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans,
          N_, K_, M_,
          (Dtype)1., top_diff, bottom_data,
          (Dtype)1., this->blobs_[0]->mutable_cpu_diff());
    }
  }
  if (bias_term_ && this->param_propagate_down_[1]) {
    const Dtype* top_diff = top[0]->cpu_diff();
    // Gradient with respect to bias
    caffe_cpu_gemv<Dtype>(CblasTrans, M_, N_, (Dtype)1., top_diff,
        bias_multiplier_.cpu_data(), (Dtype)1.,
        this->blobs_[1]->mutable_cpu_diff());
  }
  if (propagate_down[0]) {
    const Dtype* top_diff = top[0]->cpu_diff();
    // Gradient with respect to bottom data
    if (transpose_) {
      caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasTrans,
          M_, K_, N_,
          (Dtype)1., top_diff, this->blobs_[0]->cpu_data(),
          (Dtype)0., bottom[0]->mutable_cpu_diff());
    } else {
      caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans,
          M_, K_, N_,
          (Dtype)1., top_diff, this->blobs_[0]->cpu_data(),
          (Dtype)0., bottom[0]->mutable_cpu_diff());
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(InnerProductLayer);
#endif

INSTANTIATE_CLASS(InnerProductLayer);
REGISTER_LAYER_CLASS(InnerProduct);

}  // namespace caffe
