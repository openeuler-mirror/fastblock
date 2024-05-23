package osd

type CommonQueue struct {
    items []interface{}
}

func NewQueue() *CommonQueue {
    return &CommonQueue{
        items: []interface{}{},
    }
}

func (q *CommonQueue) Enqueue(item interface{}) {
    q.items = append(q.items, item)
}

func (q *CommonQueue) Dequeue() interface{} {
    if len(q.items) == 0 {
        return nil
    }
 
    item := q.items[0]
    q.items = q.items[1:]
    return item
}

func (q *CommonQueue) IsEmpty() bool {
    return len(q.items) == 0
}

func (q *CommonQueue) Size() int {
	return len(q.items) 
}