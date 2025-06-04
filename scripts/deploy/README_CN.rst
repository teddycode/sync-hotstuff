如何重现论文中的关键结果？
========================

步骤1 - 环境和依赖
---------------

本地环境
+++++++

- 我们假设您在工作计算机上已安装最新版本的Ansible_（工作计算机是指您的笔记本电脑/家用电脑）。
- 在您的工作计算机上，您已克隆最新的仓库并更新了所有子模块（如果不确定，请运行``git submodule update --init
  --recursive``）。最后，您已经构建了仓库，因此二进制文件``hotstuff-keygen``和``hotstuff-tls-keygen``在仓库根目录中可用。
- 现在，您应该在shell中位于``/scripts/deploy``目录下（``cd
  <path-to-your-libhotstuff-repo>/scripts/deploy``）。

远程环境
+++++++

- 在本示例中，我们使用Amazon EC2上的典型Linux镜像，Ubuntu 18.04。但一般来说，任何安装了Ubuntu 18.04的机器都可能有效。
- 我们假设您已经正确配置了参与实验的机器间的内部网络。这包括一些replica机器（专用于运行replica进程的机器）和若干client机器。

  - Replica机器应该能够通过从10000开始的TCP端口相互通信（默认值由``gen_conf.py``生成，可以更改）。
  - 每个client机器应该能够通过从20000开始的TCP端口与所有replica机器通信。
  - 所有机器都应该可以通过ssh私钥从您的工作计算机访问。
  - 注意：在我们的论文中，我们使用了``c5.4xlarge``以匹配我们基准测试的配置。

步骤2 - 生成部署设置
---------------

- 编辑``replicas.txt``和``client.txt``：

  - ``replicas.txt``：每行是由一个或多个空格分隔的外部IP和本地IP。外部IP将用于工作计算机与replica机器之间的控制操作，而本地IP是用于您的replica间网络基础设施中的地址，replica通过它与其他机器建立TCP连接。
  - ``clients.txt``：每行是单个外部IP。
  - 相同的IP可以在两个文件中多次出现。在这种情况下，您将在不同进程之间共享同一台机器（由于性能原因，不建议用于replicas）。

- 通过运行``./gen_all.sh``生成``node.ini``和``hotstuff.gen.*.conf``。
- 在``group_vars/all.yml``中更改ssh密钥配置。
- 通过``./run.sh setup``在所有远程机器上构建``libhotstuff``。

步骤3 - 运行实验
-------------

- （可选）根据您的喜好更改``hotstuff.gen.conf``中的参数。
- （可选）根据您的喜好更改``group_vars/clients.yml``中的参数。
- （对于replicas）创建一个新的实验运行并启动所有replica进程：``./run.sh new myrun1``。
- （等待一段时间，直到所有replica进程稳定下来，对于像EC2这样的良好网络，10秒钟应该绰绰有余）
- （对于clients）创建一个新的实验运行并启动所有client进程：``./run_cli.sh new myrun1_cli``。
- （等待直到所有命令都提交完毕，或者您只是想结束实验）
- 要收集结果，依次运行``./run_cli.sh stop myrun1_cli``和``./run_cli.sh fetch myrun1_cli``。
- 要分析结果，运行``cat myrun1_cli/remote/*/log/stderr | python ../thr_hist.py``。

  - 使用``c5.4xlarge``上的所有默认设置，我得到了以下结果：

    ::

        [349669, 367520, 371855, 370391, 366159, 367565, 365957, 322690]
        lat = 6.955ms # 平均端到端延迟
        lat = 6.970ms # 移除异常值后

- 最后，停止replicas：``./run.sh stop myrun1``。

其他注意事项
---------

- 每次``./run.sh new``（对于``./run_cli.sh``也一样）都将创建一个包含该运行的所有内容（选择的参数，原始结果）的文件夹。一个好的做法是为不同的运行总是使用一个新的名称，这样您可以很好地保留所有以前的实验。
- ``run.sh``脚本不会检测是否有其他未完成的运行（但是，它确实可以防止您扰乱同一个运行的状态，给定像"myrun1"这样的id），因此您需要确保始终``stop``（优雅退出并且所有结果都可用）或``reset``（简单地杀死所有进程）任何历史运行以重新开始。
- 要检查进程是否仍然活着：``./run.sh check myrun1``。


.. _Ansible: https://docs.ansible.com/ansible/latest/installation_guide/intro_installation.html
