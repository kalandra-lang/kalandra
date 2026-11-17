from "std/gpu" import dispatch, WorkItem

fn tiled(a: Tensor[f32, 7, 8], b: Tensor[f32, 8, 5]) -> Tensor[f32, 7, 5]:
  return dispatch([7, 5], [4, 4], fn(item: WorkItem) -> f32:
    localRow = item.localIndex(0)
    localColumn = item.localIndex(1)
    lane = localRow * 4 + localColumn
    row = if item.globalIndex(0) < 7: item.globalIndex(0)
    else: 6
    column = if item.globalIndex(1) < 5: item.globalIndex(1)
    else: 4
    left = item.shared[f32](16)
    right = item.shared[f32](16)
    total = f32(0)
    for stage in range(2):
      left[lane] = a[row, stage * 4 + localColumn]
      right[lane] = b[stage * 4 + localRow, column]
      item.barrier()
      for k in range(4):
        total = total + left[localRow * 4 + k] * right[k * 4 + localColumn]
      item.barrier()
    return total
  )
