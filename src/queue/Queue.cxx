/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Queue.hxx"
#include "song/DetachedSong.hxx"

Queue::Queue(unsigned _max_length) noexcept
	:max_length(_max_length),
	 items(new Item[max_length]),
	 order(new unsigned[max_length]),
	 id_table(max_length * HASH_MULT)
{
}

Queue::~Queue() noexcept
{
	Clear();

	delete[] items;
	delete[] order;
}

int
Queue::GetNextOrder(unsigned _order) const noexcept
{
	assert(_order < length);

	if (single != SingleMode::OFF && repeat && !consume)
		return _order;
	else if (_order + 1 < length)
		return _order + 1;
	else if (repeat && (_order > 0 || !consume))
		/* restart at first song */
		return 0;
	else
		/* end of queue */
		return -1;
}

void
Queue::IncrementVersion() noexcept
{
	static unsigned long max = (uint32_t(1) << 31) - 1;

	version++;

	if (version >= max) {
		for (unsigned i = 0; i < length; i++)
			items[i].version = 0;

		version = 1;
	}
}

void
Queue::ModifyAtOrder(unsigned _order) noexcept
{
	assert(_order < length);

	unsigned position = order[_order];
	ModifyAtPosition(position);
}

unsigned
Queue::Append(DetachedSong &&song, uint8_t priority) noexcept
{
	assert(!IsFull());

	const unsigned position = length++;
	const unsigned id = id_table.Insert(position);

	auto &item = items[position];
	item.song = new DetachedSong(std::move(song));
	item.id = id;
	item.version = version;
	item.priority = priority;

	order[position] = position;

	return id;
}

void
Queue::SwapPositions(unsigned position1, unsigned position2) noexcept
{
	unsigned id1 = items[position1].id;
	unsigned id2 = items[position2].id;

	std::swap(items[position1], items[position2]);

	items[position1].version = version;
	items[position2].version = version;

	id_table.Move(id1, position2);
	id_table.Move(id2, position1);
}

void
Queue::MovePostion(unsigned from, unsigned to) noexcept
{
	const Item tmp = items[from];

	/* move songs to one less in from->to */

	for (unsigned i = from; i < to; i++)
		MoveItemTo(i + 1, i);

	/* move songs to one more in to->from */

	for (unsigned i = from; i > to; i--)
		MoveItemTo(i - 1, i);

	/* put song at _to_ */

	id_table.Move(tmp.id, to);
	items[to] = tmp;
	items[to].version = version;

	/* now deal with order */

	if (random) {
		for (unsigned i = 0; i < length; i++) {
			if (order[i] > from && order[i] <= to)
				order[i]--;
			else if (order[i] < from &&
				 order[i] >= to)
				order[i]++;
			else if (from == order[i])
				order[i] = to;
		}
	}
}

void
Queue::MoveRange(unsigned start, unsigned end, unsigned to) noexcept
{
	const auto tmp = std::make_unique<Item[]>(end - start);

	// Copy the original block [start,end-1]
	for (unsigned i = start; i < end; i++)
		tmp[i - start] = items[i];

	// If to > start, we need to move to-start items to start, starting from end
	for (unsigned i = end; i < end + to - start; i++)
		MoveItemTo(i, start + i - end);

	// If to < start, we need to move start-to items to newend (= end + to - start), starting from to
	// This is the same as moving items from start-1 to to (decreasing), with start-1 going to end-1
	// We have to iterate in this order to avoid writing over something we haven't yet moved
	for (int i = start - 1; i >= int(to); i--)
		MoveItemTo(i, i + end - start);

	// Copy the original block back in, starting at to.
	for (unsigned i = start; i< end; i++)
	{
		id_table.Move(tmp[i - start].id, to + i - start);
		items[to + i - start] = tmp[i-start];
		items[to + i - start].version = version;
	}

	if (random) {
		// Update the positions in the queue.
		// Note that the ranges for these cases are the same as the ranges of
		// the loops above.
		for (unsigned i = 0; i < length; i++) {
			if (order[i] >= end && order[i] < to + end - start)
				order[i] -= end - start;
			else if (order[i] < start &&
				 order[i] >= to)
				order[i] += end - start;
			else if (start <= order[i] && order[i] < end)
				order[i] += to - start;
		}
	}
}

unsigned
Queue::MoveOrder(unsigned from_order, unsigned to_order) noexcept
{
	assert(from_order < length);
	assert(to_order <= length);

	const unsigned from_position = OrderToPosition(from_order);

	if (from_order < to_order) {
		for (unsigned i = from_order; i < to_order; ++i)
			order[i] = order[i + 1];
	} else {
		for (unsigned i = from_order; i > to_order; --i)
			order[i] = order[i - 1];
	}

	order[to_order] = from_position;
	return to_order;
}

unsigned
Queue::MoveOrderBefore(unsigned from_order, unsigned to_order) noexcept
{
	/* if "from_order" comes before "to_order", then the new
	   position is "to_order-1"; otherwise the "to_order" song is
	   moved one ahead */
	return MoveOrder(from_order, to_order - (from_order < to_order));
}

unsigned
Queue::MoveOrderAfter(unsigned from_order, unsigned to_order) noexcept
{
	/* if "from_order" comes after "to_order", then the new
	   position is "to_order+1"; otherwise the "to_order" song is
	   moved one back */
	return MoveOrder(from_order, to_order + (from_order > to_order));
}

void
Queue::DeletePosition(unsigned position) noexcept
{
	assert(position < length);

	delete items[position].song;

	const unsigned id = PositionToId(position);
	const unsigned _order = PositionToOrder(position);

	--length;

	/* release the song id */

	id_table.Erase(id);

	/* delete song from songs array */

	for (unsigned i = position; i < length; i++)
		MoveItemTo(i + 1, i);

	/* delete the entry from the order array */

	for (unsigned i = _order; i < length; i++)
		order[i] = order[i + 1];

	/* readjust values in the order array */

	for (unsigned i = 0; i < length; i++)
		if (order[i] > position)
			--order[i];
}

void
Queue::Clear() noexcept
{
	for (unsigned i = 0; i < length; i++) {
		Item *item = &items[i];

		delete item->song;

		id_table.Erase(item->id);
	}

	length = 0;
}

static void
queue_sort_order_by_priority(Queue *queue,
			     unsigned start, unsigned end) noexcept
{
	assert(queue != nullptr);
	assert(queue->random);
	assert(start <= end);
	assert(end <= queue->length);

	auto cmp = [queue](unsigned a_pos, unsigned b_pos){
		const Queue::Item &a = queue->items[a_pos];
		const Queue::Item &b = queue->items[b_pos];

		return a.priority > b.priority;
	};

	std::stable_sort(queue->order + start, queue->order + end, cmp);
}

void
Queue::ShuffleOrderRange(unsigned start, unsigned end) noexcept
{
	assert(random);
	assert(start <= end);
	assert(end <= length);

	rand.AutoCreate();
	std::shuffle(order + start, order + end, rand);
}

/**
 * Sort the "order" of items by priority, and then shuffle each
 * priority group.
 */
void
Queue::ShuffleOrderRangeWithPriority(unsigned start, unsigned end) noexcept
{
	assert(random);
	assert(start <= end);
	assert(end <= length);

	if (start == end)
		return;

	/* first group the range by priority */
	queue_sort_order_by_priority(this, start, end);

	/* now shuffle each priority group */
	unsigned group_start = start;
	uint8_t group_priority = GetOrderPriority(start);

	for (unsigned i = start + 1; i < end; ++i) {
		const uint8_t priority = GetOrderPriority(i);
		assert(priority <= group_priority);

		if (priority != group_priority) {
			/* start of a new group - shuffle the one that
			   has just ended */
			ShuffleOrderRange(group_start, i);
			group_start = i;
			group_priority = priority;
		}
	}

	/* shuffle the last group */
	ShuffleOrderRange(group_start, end);
}

void
Queue::ShuffleOrder() noexcept
{
	ShuffleOrderRangeWithPriority(0, length);
}

void
Queue::ShuffleOrderFirst(unsigned start, unsigned end) noexcept
{
	rand.AutoCreate();

	std::uniform_int_distribution<unsigned> distribution(start, end - 1);
	SwapOrders(start, distribution(rand));
}

void
Queue::ShuffleOrderLastWithPriority(unsigned start, unsigned end) noexcept
{
	assert(end <= length);
	assert(start < end);

	/* skip all items at the start which have a higher priority,
	   because the last item shall only be shuffled within its
	   priority group */
	const auto last_priority = items[OrderToPosition(end - 1)].priority;
	while (items[OrderToPosition(start)].priority != last_priority) {
		++start;
		assert(start < end);
	}

	rand.AutoCreate();

	std::uniform_int_distribution<unsigned> distribution(start, end - 1);
	SwapOrders(end - 1, distribution(rand));
}

void
Queue::ShuffleRange(unsigned start, unsigned end) noexcept
{
	assert(start <= end);
	assert(end <= length);

	rand.AutoCreate();

	for (unsigned i = start; i < end; i++) {
		std::uniform_int_distribution<unsigned> distribution(start,
								     end - 1);
		unsigned ri = distribution(rand);
		SwapPositions(i, ri);
	}
}

unsigned
Queue::FindPriorityOrder(unsigned start_order, uint8_t priority,
			 unsigned exclude_order) const noexcept
{
	assert(random);
	assert(start_order <= length);

	for (unsigned i = start_order; i < length; ++i) {
		const unsigned position = OrderToPosition(i);
		const Item *item = &items[position];
		if (item->priority <= priority && i != exclude_order)
			return i;
	}

	return length;
}

unsigned
Queue::CountSamePriority(unsigned start_order, uint8_t priority) const noexcept
{
	assert(random);
	assert(start_order <= length);

	for (unsigned i = start_order; i < length; ++i) {
		const unsigned position = OrderToPosition(i);
		const Item *item = &items[position];
		if (item->priority != priority)
			return i - start_order;
	}

	return length - start_order;
}

bool
Queue::SetPriority(unsigned position, uint8_t priority, int after_order,
		   bool reorder) noexcept
{
	assert(position < length);

	Item *item = &items[position];
	uint8_t old_priority = item->priority;
	if (old_priority == priority)
		return false;

	item->version = version;
	item->priority = priority;

	if (!random || !reorder)
		/* don't reorder if not in random mode */
		return true;

	unsigned _order = PositionToOrder(position);
	if (after_order >= 0) {
		if (_order == unsigned(after_order))
			/* don't reorder the current song */
			return true;

		if (_order < unsigned(after_order)) {
			/* the specified song has been played already
			   - enqueue it only if its priority has been
			   increased and is now bigger than the
			   current one's */

			const unsigned after_position =
				OrderToPosition(after_order);
			const Item *after_item =
				&items[after_position];
			if (priority <= old_priority ||
			    priority <= after_item->priority)
				/* priority hasn't become bigger */
				return true;
		}
	}

	/* move the item to the beginning of the priority group (or
	   create a new priority group) */

	const unsigned before_order =
		FindPriorityOrder(after_order + 1, priority, _order);
	const unsigned new_order = before_order > _order
		? before_order - 1
		: before_order;
	MoveOrder(_order, new_order);

	/* shuffle the song within that priority group */

	const unsigned priority_count = CountSamePriority(new_order, priority);
	assert(priority_count >= 1);
	ShuffleOrderFirst(new_order, new_order + priority_count);

	return true;
}

bool
Queue::SetPriorityRange(unsigned start_position, unsigned end_position,
			uint8_t priority, int after_order) noexcept
{
	assert(start_position <= end_position);
	assert(end_position <= length);

	bool modified = false;
	int after_position = after_order >= 0
		? int(OrderToPosition(after_order))
		: -1;
	for (unsigned i = start_position; i < end_position; ++i) {
		after_order = after_position >= 0
			? int(PositionToOrder(after_position))
			: -1;

		modified |= SetPriority(i, priority, after_order);
	}

	return modified;
}
