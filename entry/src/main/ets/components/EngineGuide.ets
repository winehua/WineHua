import { WineEnvService } from '../service/WineEnvService';
import { EmptyState } from './EmptyState';

// 引擎未就绪共享引导 (应用库/运行中/文件三 tab 复用, 替代各处自绘空态)。
// 可见性由调用方按场景判定 (!prefixReady / 列表空且引擎未跑), 本组件只按
// 引擎状态渲染五种形态:
//   busy:           编排进行中转圈 (多数时候被 init overlay 盖住, 此处兜底);
//   failed:         失败详情 + 重试/重置环境 (动作与 init overlay 失败分支同源,
//                   overlay 经"返回启动器"关闭后由此承接自救);
//   ready-degraded: 桌面未就绪, 给"重启引擎"自救 (同设置页策略);
//   ready:          prefix 后台轮询中, 无按钮 (此时给"启动"会触发重启编排);
//   idle:           "启动引擎"按钮。desc 分首次/二次进入 (hasPrefixOnDisk):
//                   首次含解压数据+建 prefix, 提前给耗时预期。
@Component
export struct EngineGuide {
  @State state: string = 'idle';
  @State busy: boolean = false;
  @State failedDetail: string = '';

  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.state = this.env.engineState;
    this.busy = this.env.isBusy();
    this.failedDetail = this.env.initFailed ? this.env.initOverlayText : '';
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirror);
    this.mirror();
  }

  aboutToDisappear(): void {
    // 本组件随引擎状态反复挂载/卸载, 必须退订防 listeners 堆积
    this.env.unsubscribe(this.mirror);
  }

  private onStartEngine(): void {
    // isBusy 防重入 (restartEngine 内部也有, 双保险)
    if (this.env.isBusy()) {
      return;
    }
    this.env.restartEngine();
  }

  private confirmReset(): void {
    // 与 Index init overlay 失败分支同款确认与动作
    AlertDialog.show({
      title: '重置 Wine 引擎',
      message: '将清空整个 Wine 容器（包括所有已安装程序和存档），且不可恢复。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '重置',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.env.doReset();
        }
      }
    });
  }

  build() {
    Column({ space: 4 }) {
      EmptyState({
        icon: this.state == 'failed'
          ? $r('sys.symbol.exclamationmark_triangle')
          : $r('sys.symbol.play_circle'),
        title: this.busy
          ? (this.state == 'stopping' ? '正在停止引擎...' : '正在初始化引擎...')
          : this.state == 'failed' ? '引擎启动失败'
          : this.state == 'ready-degraded' ? '桌面未能完全就绪'
          : this.state == 'ready' ? '正在准备环境...'
          : '引擎未启动',
        desc: this.busy
          ? (this.env.hasPrefixOnDisk() ? '请稍候...' : '首次启动需解压数据并创建环境，请稍候')
          : this.state == 'failed'
            ? (this.failedDetail ? this.failedDetail : '可重试，或重置环境后重新初始化')
          : this.state == 'ready-degraded' ? '可尝试重启引擎'
          : this.state == 'ready' ? '环境初始化完成后即可使用'
          : (this.env.hasPrefixOnDisk()
            ? '启动引擎后即可浏览应用与文件'
            : '首次启动需解压数据并初始化环境，约需 1 分钟；之后启动只需几秒')
      })
      if (this.busy) {
        LoadingProgress()
          .width(18).height(18)
          .color($r('app.color.accent'))
          .margin({ top: 14 })
      } else if (this.state == 'failed') {
        Row({ space: 12 }) {
          Button('重试')
            .fontColor(Color.White)
            .onClick(() => this.onStartEngine())
          Button('重置环境')
            .backgroundColor($r('app.color.action_danger'))
            .fontColor(Color.White)
            .onClick(() => this.confirmReset())
        }
        .margin({ top: 14 })
      } else if (this.state == 'ready-degraded') {
        Button('重启引擎')
          .fontColor(Color.White)
          .margin({ top: 14 })
          .onClick(() => this.onStartEngine())
      } else if (this.state == 'idle') {
        Button('启动引擎')
          .fontColor(Color.White)
          .margin({ top: 14 })
          .onClick(() => this.onStartEngine())
      }
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor($r('app.color.window_bg'))
  }
}
