/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "gtest/gtest.h"
#include "mkldnn_test_common.hpp"

#include "mkldnn.hpp"

namespace mkldnn {

template <typename T> T relu_fwd(T s, T alpha) {
    return s > 0 ? s : s * alpha;
}
template <typename T> T relu_bwd(T dd, T s, T alpha) {
    return s > 0 ? dd : dd * alpha;
}

template <typename T> T tanh_fwd(T s) {
    T e = ::expf(2*s); /* maybe replace with -2*s? */
    return (e - 1) / (e + 1);
}
template <typename T> T tanh_bwd(T dd, T s) {
    T th = tanh_fwd(s);
    return dd * (1 - th * th);
}

template <typename T, typename A> T elu_fwd(T s, A alpha) {
    return s > 0 ? s : alpha * (::expf(s) - 1);
}
template <typename T, typename A> T elu_bwd(T dd, T s, A alpha) {
    return dd * (s > 0 ? 1. : alpha * ::expf(s));
}

template <typename data_t>
struct eltwise_test_params {
    engine::kind engine_kind;
    algorithm alg_kind;
    memory::format data_format;
    memory::format diff_format;
    data_t alpha, beta;
    memory::dims dims;
};

template <typename data_t>
void check_eltwise_fwd(const eltwise_test_params<data_t> &p,
        const memory::desc &md, const memory &src, const memory &dst)
{
    data_t *src_data = (data_t *)src.get_data_handle();
    data_t *dst_data = (data_t *)dst.get_data_handle();

    ASSERT_EQ(md.data.ndims, 4);
    ASSERT_EQ(md.data.data_type, memory::convert_to_c(memory::data_type::f32)); // TODO: type assert

    size_t N = md.data.dims[0];
    size_t C = md.data.dims[1];
    size_t H = md.data.dims[2];
    size_t W = md.data.dims[3];
    for (size_t i = 0; i < N * C * H * W; ++i) {
        data_t s = src_data[i];
        data_t ref_d = 0;
        switch (p.alg_kind) {
        case eltwise_relu: ref_d = relu_fwd(s, p.alpha); break;
        case eltwise_tanh: ref_d = tanh_fwd(s); break;
        case eltwise_elu: ref_d = elu_fwd(s, p.alpha); break;
        default: assert(!"unknown alg_kind");
        }
        EXPECT_NEAR(dst_data[i], ref_d, 1.e-6);
    }
}

template <typename data_t>
void check_eltwise_bwd(const eltwise_test_params<data_t> &p,
        const memory::desc &md, const memory &src, const memory &diff_dst,
        const memory &diff_src)
{
    data_t *src_data = (data_t *)src.get_data_handle();
    data_t *diff_dst_data = (data_t *)diff_dst.get_data_handle();
    data_t *diff_src_data = (data_t *)diff_src.get_data_handle();

    const memory::desc data_d = src.get_primitive_desc().desc();
    const memory::desc diff_data_d = diff_src.get_primitive_desc().desc();

    ASSERT_EQ(md.data.ndims, 4);
    ASSERT_EQ(md.data.data_type, memory::convert_to_c(memory::data_type::f32)); // TODO: type assert

    size_t N = md.data.dims[0];
    size_t C = md.data.dims[1];
    size_t H = md.data.dims[2];
    size_t W = md.data.dims[3];
    for (size_t i = 0; i < N * C * H * W; ++i) {
        data_t ref_s = src_data[map_index(data_d, i)];
        data_t ref_dd = diff_dst_data[map_index(diff_data_d, i)];
        data_t ref_ds = 0;
        switch (p.alg_kind) {
        case eltwise_relu: ref_ds = relu_bwd(ref_dd, ref_s, p.alpha); break;
        case eltwise_tanh: ref_ds = tanh_bwd(ref_dd, ref_s); break;
        case eltwise_elu: ref_ds = elu_bwd(ref_dd, ref_s, p.alpha); break;
        default: assert(!"unknown alg_kind");
        }
        EXPECT_NEAR(diff_src_data[map_index(diff_data_d, i)], ref_ds, 1.e-6);
    }
}

template <typename data_t>
class eltwise_test : public ::testing::TestWithParam<eltwise_test_params<data_t>> {
private:
    std::shared_ptr<memory> src;
    std::shared_ptr<memory> diff_src;
    std::shared_ptr<memory> dst;
    std::shared_ptr<memory> diff_dst;
    std::shared_ptr<memory> workspace;
    std::shared_ptr<memory::desc> data_desc;
    std::shared_ptr<memory::desc> diff_data_desc;
    std::shared_ptr<eltwise_forward::primitive_desc> eltwise_prim_desc;
    eltwise_test_params<data_t> p;
    std::shared_ptr<engine> eng;
    memory::data_type data_type;
    int size;

protected:
    virtual void SetUp() {
        p = ::testing::TestWithParam<eltwise_test_params<data_t>>::GetParam();

        ASSERT_TRUE(p.engine_kind == engine::kind::cpu);
        eng.reset(new engine(p.engine_kind, 0));

        ASSERT_EQ(p.dims.size(), 4U);

        data_type = data_traits<data_t>::data_type;
        ASSERT_EQ(data_type, mkldnn::memory::data_type::f32);

        size = p.dims[0] * p.dims[1] * p.dims[2] * p.dims[3];

        Forward();
        Backward();
    }

    void Forward() {
        data_desc.reset(new memory::desc(p.dims, data_type,
            p.data_format));
        diff_data_desc.reset(new memory::desc(p.dims, data_type,
            p.diff_format));
        src.reset(new memory({*data_desc, *eng}));
        dst.reset(new memory({*data_desc, *eng}));

        fill_data<data_t>(size, (data_t *)src->get_data_handle(),
                data_t(0), data_t(1));

        auto eltwise_desc = eltwise_forward::desc(prop_kind::forward_training,
                p.alg_kind, *data_desc, p.alpha, p.beta);
        eltwise_prim_desc.reset(
                new eltwise_forward::primitive_desc(eltwise_desc, *eng));
        auto eltwise = eltwise_forward(*eltwise_prim_desc, *src, *dst);

        std::vector<primitive> pipeline;
        pipeline.push_back(eltwise);
        auto s = stream(stream::kind::lazy);
        s.submit(pipeline).wait();

        check_eltwise_fwd(p, *data_desc, *src, *dst);
    }

    void Backward() {
        diff_src.reset(new memory({*diff_data_desc, *eng}));
        diff_dst.reset(new memory({*diff_data_desc, *eng}));

        fill_data<data_t>(size, (data_t *)diff_dst->get_data_handle(),
                data_t(0), data_t(1));

        auto eltwise_bwd_desc = eltwise_backward::desc(p.alg_kind,
                *diff_data_desc, *data_desc, p.alpha, p.beta);
        auto eltwise_bwd_prim_desc = eltwise_backward::primitive_desc(
                eltwise_bwd_desc, *eng, *eltwise_prim_desc);
        auto eltwise_bwd = eltwise_backward(eltwise_bwd_prim_desc, *src,
                *diff_dst, *diff_src);

        std::vector<primitive> pipeline;
        pipeline.push_back(eltwise_bwd);
        auto s = stream(stream::kind::lazy);
        s.submit(pipeline).wait();

        check_eltwise_bwd(p, *data_desc, *src, *diff_dst, *diff_src);
    }
};

using eltwise_test_float = eltwise_test<float>;
using eltwise_test_params_float = eltwise_test_params<float>;

TEST_P(eltwise_test_float, TestsEltwise)
{
}

#define EXPAND(args) args

#define EXPAND_FORMATS(data) memory::format::data

#define ENGINE engine::kind::cpu

#define PARAMS(alg, data, diff_data, alpha, beta, mb, c, h, w) \
    eltwise_test_params_float { ENGINE, algorithm::alg, \
    EXPAND_FORMATS(data), EXPAND_FORMATS(diff_data), \
    alpha, beta, {mb, c, h, w} }

#define PARAMS_ALL_ALG(...) \
    EXPAND(PARAMS(eltwise_relu, __VA_ARGS__)), \
    EXPAND(PARAMS(eltwise_tanh, __VA_ARGS__)), \
    EXPAND(PARAMS(eltwise_elu, __VA_ARGS__))

#define INST_TEST_CASE(str, ...) INSTANTIATE_TEST_CASE_P( \
        str, eltwise_test_float, ::testing::Values(__VA_ARGS__))

INST_TEST_CASE(SimpleZeroNegativeSlope_NCHW,
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 8, 4, 4),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 16, 4, 4),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 16, 8, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 16, 16, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 16, 10, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 10, 10, 10, 10),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 256, 64, 8, 16),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 1, 1, 1, 1),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 3, 5, 7, 11)
);

INST_TEST_CASE(Simple_NCHW,
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 8, 4, 4),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 16, 4, 4),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 16, 8, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 16, 16, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 16, 10, 8),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 10, 10, 10, 10),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 256, 64, 8, 16),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 1, 1, 1, 1),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 3, 5, 7, 11)
);

INST_TEST_CASE(Simple,
    PARAMS_ALL_ALG(nchw, nChw8c, 0.1f, 0.f, 2, 8, 4, 4),
    PARAMS_ALL_ALG(nChw8c, nchw, 0.1f, 0.f, 2, 16, 4, 4),
    PARAMS_ALL_ALG(nchw, nchw, 0.1f, 0.f, 2, 16, 8, 8),
    PARAMS_ALL_ALG(nChw8c, nChw8c, 0.1f, 0.f, 2, 16, 16, 8),
    PARAMS_ALL_ALG(nhwc, nchw, 0.1f, 0.f, 2, 16, 10, 8),
    PARAMS_ALL_ALG(nchw, nhwc, 0.1f, 0.f, 10, 10, 10, 10)
);

INST_TEST_CASE(AlexNet_NCHW,
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 96, 55, 55),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 256, 27, 27),
    PARAMS_ALL_ALG(nchw, nchw, 0.f, 0.f, 2, 384, 13, 13)
);

}