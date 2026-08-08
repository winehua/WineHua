// 统一空态组件: SymbolGlyph 图标 + 标题 + 可选说明。
// 应用库/运行中/文件页三处空态收敛到同一组件, 避免各处样式漂移;
// 动作按钮不内建 (各处动作数量不一), 由调用方在本组件下方自行组合。
// 注: 不用系统 ExceptionPrompt — 它语义偏网络异常。
@Component
export struct EmptyState {
  // 必须 @Prop (父组件单向同步): 普通成员变量只在首次构造时赋值,
  // 父组件重渲染若复用本实例 (同一 if 分支内), 文案会停留在旧值 —
  // 应用库五分类切换空态时实测踩到 (切到"外部导入"仍显示"最近使用"文案)
  // 系统符号资源, 如 $r('sys.symbol.square_grid_2x2'); 由调用方按场景指定
  @Prop icon: Resource = $r('sys.symbol.inbox');
  @Prop title: string = '';
  @Prop desc: string = '';

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
