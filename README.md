# Caffe

[![Build Status](https://travis-ci.org/BVLC/caffe.svg?branch=master)](https://travis-ci.org/BVLC/caffe)
[![License](https://img.shields.io/badge/license-BSD-blue.svg)](LICENSE)

Caffe is a deep learning framework made with expression, speed, and modularity in mind.
It is developed by the Berkeley Vision and Learning Center ([BVLC](http://bvlc.eecs.berkeley.edu)) and community contributors.

Check out the [project site](http://caffe.berkeleyvision.org) for all the details like

- [DIY Deep Learning for Vision with Caffe](https://docs.google.com/presentation/d/1UeKXVgRvvxg9OUdh_UiC5G71UMscNPlvArsWER41PsU/edit#slide=id.p)
- [Tutorial Documentation](http://caffe.berkeleyvision.org/tutorial/)
- [BVLC reference models](http://caffe.berkeleyvision.org/model_zoo.html) and the [community model zoo](https://github.com/BVLC/caffe/wiki/Model-Zoo)
- [Installation instructions](http://caffe.berkeleyvision.org/installation.html)

and step-by-step examples.

[![Join the chat at https://gitter.im/BVLC/caffe](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/BVLC/caffe?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Please join the [caffe-users group](https://groups.google.com/forum/#!forum/caffe-users) or [gitter chat](https://gitter.im/BVLC/caffe) to ask questions and talk about methods and models.
Framework development discussions and thorough bug reports are collected on [Issues](https://github.com/BVLC/caffe/issues).

Happy brewing!

## License and Citation

Caffe is released under the [BSD 2-Clause license](https://github.com/BVLC/caffe/blob/master/LICENSE).
The BVLC reference models are released for unrestricted use.

Please cite Caffe in your publications if it helps your research:

    @article{jia2014caffe,
      Author = {Jia, Yangqing and Shelhamer, Evan and Donahue, Jeff and Karayev, Sergey and Long, Jonathan and Girshick, Ross and Guadarrama, Sergio and Darrell, Trevor},
      Journal = {arXiv preprint arXiv:1408.5093},
      Title = {Caffe: Convolutional Architecture for Fast Feature Embedding},
      Year = {2014}
    }

## SkimCaffe Specific Description

We assume you have a recent Intel compiler and MKL installed.
Tested environments: (Intel compiler version 15.0.3.187 and boost 1.59.0)
We also assume you have a recent x86 CPU with AVX2 or AVX512 support.
Direct sparse convolution and sparse fully-connected layers is only tested for AlexNet.
More details on direct sparse convolution is described at: https://arxiv.org/abs/1608.01409

1) Set up Intel compiler environment (compilervars.sh or compilervars.csh)

2) Compile SpMP:

```
cd experiments/sparsity/SpMP
make
```

3) Build Caffe as usual

4) Test:

```
bzip2 -d models/bvlc_reference_caffenet/fc_0.1_ft_caffenet_0.57368_5e-05.caffemodel.bz2
build/tools/caffe.bin test -model models/bvlc_reference_caffenet/test_direct_sconv.prototxt -weights models/bvlc_reference_caffenet/fc_0.1_ft_caffenet_0.57368_5e-05.caffemodel -iterations 3
```
