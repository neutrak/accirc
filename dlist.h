//printf
#include <stdio.h>

//memcpy
#include <string.h>

//the basic unit (cons cell) for a doubly-linked list
typedef struct dlist_entry {
	struct dlist_entry *prev;
	struct dlist_entry *next;
	
	//NOTE: this stores a generic pointer to any data
	//it is up to the calling code to remember the type of the data it's using, and deref accordingly!
	void *data;
} dlist_entry;

//append new data to the given dlist
//NOTE: creating a new dlist is the same as appending to a NULL pointer (a NULL pointer and an empty dlist are the same thing)
dlist_entry *dlist_append(dlist_entry *list, void *data){
	//create the new data entry regardless of the existing list
	dlist_entry *new_entry=malloc(sizeof(dlist_entry));
	if(new_entry==NULL){
		printf("Err: malloc() call failed; out of memory!?\n");
		exit(1);
		return NULL;
	}
	new_entry->prev=NULL;
	new_entry->next=NULL;
	new_entry->data=data;
	
	//if there was no existing list then just return this entry
	//and we're done!
	if(list==NULL){
		return new_entry;
	}
	
	//otherwise...
	//go to the end of the list and find the place to add this entry
	dlist_entry *list_end=list;
	while(list_end->next!=NULL){
		list_end=list_end->next;
	}
	list_end->next=new_entry;
	
	//remember to save a prev pointer because this is a DOUBLY linked list, not a singly linked list :)
	//NOTE: this points to the END of the previously-existing list, not the START, which is the return of this function
	new_entry->prev=list_end;
	
	//return the original unmodified list pointer since that's still the /start/ of the list
	return list;
}

//free a single dlist entry without affecting to rest of the list
void dlist_free_entry(dlist_entry *item, char free_data){
	//get its next and prev components
	dlist_entry *prev=item->prev;
	dlist_entry *next=item->next;
	
	//update pointers to skip this data in the list
	if(prev!=NULL){
		prev->next=next;
	}
	if(next!=NULL){
		next->prev=prev;
	}
	item->prev=NULL;
	item->next=NULL;
	
	//now that references have been removed and this entry has been de-coupled
	//free the item's data if requested
	if(free_data){
		free(item->data);
	}
	//free the item
	free(item);
}

//free an ENTIRE dlist
void dlist_free(dlist_entry *list, char free_data){
	dlist_entry *next=NULL;
	while(list!=NULL){
		next=list->next;
		dlist_free_entry(list,free_data);
		list=next;
	}
}


//get the item at the given index out of the list
dlist_entry *dlist_get_entry(dlist_entry *list, int idx) {
	//if the index given was negative, then ignore his request
	if(idx<0){
		return NULL;
	}
	
	int n;
	for(n=0;n<idx;n++){
		if(list==NULL){
			return NULL;
		}
		list=list->next;
	}
	return list;
}

//get the number if items contained in the given list
//given a pointer to the start of the list
int dlist_length(dlist_entry *list){
	int length=0;
	while(list!=NULL){
		list=list->next;
		length++;
	}
	return length;
}

//delete the item at the given index from the list
//NOTE: this has to return a new list pointer in case the idx given was 0 (in which case we delete the first item!)
dlist_entry *dlist_delete_entry(dlist_entry *list, int idx, char free_data) {
	dlist_entry *ret=list;
	
	//get the item to delete
	dlist_entry *item=dlist_get_entry(list,idx);
	if(item!=NULL){
		//if we're deleting the 0th item
		//then our return value will be the formerly-1st item
		//which will be the new 0th item
		if(item==ret){
			//NOTE: a 0-length list and a NULL pointer are the same thing
			ret=ret->next;
		}
//		printf("[dbg] dlist_free_entry being called on index %i, item=%p\n",idx,item);
		dlist_free_entry(item,free_data);
	}
	return ret;
}

//perform a data-wise copy of the given dlist
//NOTE: this allocates new memory!
dlist_entry *dlist_deep_copy(dlist_entry *old_list, unsigned int data_size){
	dlist_entry *new_list=NULL;
	while(old_list!=NULL){
		void *old_data=old_list->data;
		void *new_data=malloc(data_size);
		if(new_data==NULL){
			printf("Err: malloc() call failed; out of memory!?\n");
			exit(1);
			return NULL;
		}
		
		//perform a data-wise copy of the old data into the new data
		memcpy(new_data,old_data,data_size);
		
		new_list=dlist_append(new_list,new_data);
		
		old_list=old_list->next;
	}
	return new_list;
}

//move a node from the old index to the new index
//this performs a right-shift on the nodes after old_idx; it is NOT a swap operation!
//NOTE: an insertion operation can be implemented as an append followed by a move
//NOTE: list is passed as a pointer to a list because we're going to update it if old_idx=0 or new_idx=0,
//which invalidates the previous start pointer
int dlist_move_node(dlist_entry **list, int old_idx, int new_idx){
	//if the source and destination were the same
	//or if this is moving to a location that would, after the move, be shifted back
	//then this is a no-op
	if((old_idx==new_idx)||((old_idx+1)==new_idx)){
		//do nothing and return the existing index
		return old_idx;
	}
	
	dlist_entry *prev=dlist_get_entry(*list,new_idx-1);
	dlist_entry *next=dlist_get_entry(*list,new_idx);
	
	//NOTE: reconnection is known to be the last node in the list, by definition
	dlist_entry *node_to_move=dlist_get_entry(*list,old_idx);
	if(node_to_move!=NULL){
		//decouple this node from its current index by re-setting the pointers
		//for the nodes surrounding it to skip it
		if(node_to_move->prev!=NULL){
			node_to_move->prev->next=node_to_move->next;
		//if prev was null then this node to move was previously the start of the list but will no longer be
		//and the list pointer should be updated
		}else{
			*list=node_to_move->next;
		}

		if(node_to_move->next!=NULL){
			node_to_move->next->prev=node_to_move->prev;
		}
		
		//insert this node at the desired index, implicitly pushing the "next" node over one index
		node_to_move->prev=prev;
		node_to_move->next=next;
		if(prev!=NULL){
			prev->next=node_to_move;
		//if prev was null then this moved node is now the start of the list and the list pointer should be updated
		}else{
			*list=node_to_move;
		}
		if(next!=NULL){
			next->prev=node_to_move;
		}
		
		//if the node was moved successfully then return the new index where it can be found
		//correcting for the relevant shifts
		return (new_idx>old_idx)?(new_idx-1):new_idx;
	}
	
	//if the node was null then no index is valid so return -1 (garbage in, garbage out)
	return -1;
}

