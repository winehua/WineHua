import testNapi from 'libentry.so';

@Component
export struct PerformanceOverlay {
  @State private frames: string = '客户端 FPS -- | 显示 FPS --';
  @State private gpu: string = 'GPU: 初始化中';
  @State private load: string = 'CPU -- | 内存 --';
  @State private status: string = '等待新帧';
  private timer: number = -1;
  @State private collapsed: boolean = false;
  @State private dragX: number = 0;
  @State private dragY: number = 0;
  private dragStartX: number = 0;
  private dragStartY: number = 0;

  aboutToAppear(): void {
    this.refresh();
    this.timer = setInterval(() => this.refresh(), 1000);
  }

  aboutToDisappear(): void {
    if (this.timer >= 0) clearInterval(this.timer);
    this.timer = -1;
  }

  private refresh(): void {
    try {
      const stats = testNapi.getPerformanceState();
      this.frames = `客户端 ${stats.clientFps.toFixed(1)} FPS | 显示 ${stats.displayFps.toFixed(1)} FPS`;
      this.gpu = `GPU: ${stats.gpu}`;
      this.load = `CPU(宿主) ${stats.cpuPercent < 0 ? '--' : stats.cpuPercent.toFixed(0) + '%'} | RSS ${stats.memoryMb.toFixed(0)} MB`;
      this.status = `${stats.width}×${stats.height} | ${stats.clientFps > 0 ? (1000 / stats.clientFps).toFixed(1) + ' ms/帧' : '等待客户端新帧'}`;
    } catch (_) {
      this.status = '性能采样未就绪';
    }
  }

  build() {
    Column({ space: 3 }) {
      Row() {
        Text('性能 · 拖动').fontSize(10).fontColor('#C9D5E8').layoutWeight(1)
        Text(this.collapsed ? '+' : '−').fontSize(16).fontColor(Color.White)
          .width(25).height(22).textAlign(TextAlign.Center)
          .onClick(() => { this.collapsed = !this.collapsed; })
      }
      .width('100%')
      .onTouch((event: TouchEvent) => event.stopPropagation())
      .gesture(PanGesture({ direction: PanDirection.All, distance: 5 })
        .onActionStart(() => { this.dragStartX = this.dragX; this.dragStartY = this.dragY; })
        .onActionUpdate((event: GestureEvent) => {
          this.dragX = this.dragStartX + event.offsetX;
          this.dragY = this.dragStartY + event.offsetY;
        }))
      if (!this.collapsed) {
      Text(this.frames).fontSize(10).fontColor('#8FFF9E')
      Text(this.gpu).fontSize(10).fontColor(Color.White).maxLines(2)
      Text(this.load).fontSize(10).fontColor(Color.White)
      Text(this.status).fontSize(10).fontColor('#FFD480')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .padding(8).width(160).backgroundColor('#B0000000').borderRadius(6)
    .position({ x: '100%', y: '55%' })
    .translate({ x: -168 + this.dragX, y: -25 + this.dragY })
  }
}
