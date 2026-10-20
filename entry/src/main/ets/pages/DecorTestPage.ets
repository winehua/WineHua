// [[任务#129 装饰实验]] 裸装饰测试窗的内容页: 纯静态 UI, 零业务依赖
// (子窗无渲染树时整窗不合成 —— 背景 API 也不显示, 必须 loadContent 才可见)。
// 测试结论落地后随 EntryAbility 测试钩子一并删除。
@Entry
@Component
struct DecorTestPage {
  build() {
    Column() {
      Text('DECOR TEST WINDOW')
        .fontSize(30)
        .fontColor('#FFFFFF')
        .margin({ top: 60 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FF0040')
  }
}
