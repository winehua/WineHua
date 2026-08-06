// 统一空态组件: SymbolGlyph 图标 + 标题 + 可选说明。
// 应用库/运行中/文件页三处空态收敛到同一组件, 避免各处样式漂移;
// 动作按钮不内建 (各处动作数量不一), 由调用方在本组件下方自行组合。
// 注: 不用系统 ExceptionPrompt — 它语义偏网络异常。
@Component
export struct EmptyState {
  // 系统符号资源, 如 $r('sys.symbol.square_grid_2x2'); 由调用方按场景指定
  icon: Resource = $r('sys.symbol.inbox');
  title: string = '';
  desc: string = '';

  build() {
    Column({ space: 10 }) {
      SymbolGlyph(this.icon)
        .fontSize(48)
        .fontColor([$r('app.color.text_disabled')])
      Text(this.title)
        .fontSize(16)
        .fontColor($r('app.color.text_secondary'))
      if (this.desc) {
        Text(this.desc)
          .fontSize(13)
          .fontColor($r('app.color.text_tertiary'))
          .textAlign(TextAlign.Center)
          .maxLines(2)
      }
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
  }
}
